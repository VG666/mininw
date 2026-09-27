# embed-js.ps1 -- turn js\api\*.js / js\nw_node.js into C++ string constants.
#
# Why not include_str style tooling: this has to compile under both MSVC and
# zig/clang, and it MUST be chunked. MSVC silently truncates a single string
# literal past 16380 bytes (C2026); a truncated script looks like "half a file
# got injected, syntax error, nobody noticed", which is miserable to debug.
# So we slice at 8000 chars and concatenate raw literals R"NWJS(...)NWJS".
#
# NOTE: keep this file pure ASCII. PowerShell 5.1 on a zh-CN box reads a .ps1
# without a BOM as GBK, and non-ASCII bytes get mangled badly enough to break
# the parser ("Missing ')' in method call"). English-only text is immune.
param(
    [Parameter(Mandatory = $true)][string]$ApiDir,
    [Parameter(Mandatory = $true)][string]$NodeJs,
    [Parameter(Mandatory = $true)][string]$Output
)

$ErrorActionPreference = 'Stop'
$ChunkSize = 8000

# Read a file as UTF-8 and strip a leading BOM (otherwise the injected script
# starts with three stray bytes).
function Read-Utf8 {
    param([string]$Path)
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
        $bytes = $bytes[3..($bytes.Length - 1)]
    }
    return [System.Text.Encoding]::UTF8.GetString($bytes)
}

# Emit: std::string <Symbol>() { ... }
function Emit-Function {
    param([string]$Text, [string]$Symbol)
    $lines = New-Object 'System.Collections.Generic.List[string]'
    $lines.Add("std::string $Symbol() {")
    $lines.Add("    static const char* const kParts[] = {")
    for ($at = 0; $at -lt $Text.Length; $at += $ChunkSize) {
        $size = [Math]::Min($ChunkSize, $Text.Length - $at)
        $piece = $Text.Substring($at, $size)
        if ($piece.Contains(')NWJS"')) {
            throw 'script contains the raw literal terminator )NWJS" -- pick another delimiter'
        }
        $lines.Add('        R"NWJS(' + $piece + ')NWJS",')
    }
    $lines.Add("        nullptr")
    $lines.Add("    };")
    $lines.Add("    std::string out;")
    $lines.Add("    for (const char* const* part = kParts; *part; ++part) out += *part;")
    $lines.Add("    return out;")
    $lines.Add("}")
    return , $lines.ToArray()
}

# Assemble js\api\*.js into one script, exactly like the host does at runtime
# (nw_host.cpp -> AssembleApiModules): order by js\api\modules.txt, append any
# unlisted modules sorted by name, concatenate, wrap in a single IIFE with the
# idempotence guard. The prologue/epilogue MUST stay byte-identical to the copy
# in nw_host.cpp -- if they drift, the baked-in path and the on-disk path stop
# behaving the same way. Keeping the wrapper here instead of in each module is
# what lets the modules share one function scope.
# Guard name must NOT be __nmbInstalled: in bridge mode the bridge injects its own
# script first and uses __nmb* names -- a same-named guard would drop whichever
# script runs second (and with it, the whole nw API).
$apiPrologue = "(function () {`n  'use strict';`n  if (window.__nwApiInstalled) return;`n  window.__nwApiInstalled = true;`n"
$apiEpilogue = "`n})();`n"

function Get-ModuleChain {
    param([string]$Dir)
    $chainFile = Join-Path $Dir 'modules.txt'
    if (-not (Test-Path -LiteralPath $chainFile)) { return $null }
    $order = New-Object 'System.Collections.Generic.List[string]'
    foreach ($raw in (Read-Utf8 -Path $chainFile) -split "`r?`n") {
        $line = $raw
        $hash = $line.IndexOf('#')
        if ($hash -ge 0) { $line = $line.Substring(0, $hash) }
        $line = $line.Trim()
        if ($line.Length -gt 0) { [void]$order.Add($line) }
    }
    return , $order
}

function Get-ApiText {
    param([string]$Dir)
    $disk = @(Get-ChildItem -Path $Dir -Filter '*.js' -File)
    if ($disk.Count -eq 0) { throw "no *.js under $Dir" }
    $chain = Get-ModuleChain -Dir $Dir
    $modules = New-Object 'System.Collections.Generic.List[System.IO.FileInfo]'
    if ($null -eq $chain) {
        foreach ($m in ($disk | Sort-Object Name)) { [void]$modules.Add($m) }
    } else {
        $used = @{}
        foreach ($wanted in $chain) {
            $hit = $disk | Where-Object { -not $used.ContainsKey($_.Name) -and $_.Name -ieq $wanted } | Select-Object -First 1
            if ($hit) { [void]$modules.Add($hit); $used[$hit.Name] = $true }
        }
        foreach ($m in ($disk | Where-Object { -not $used.ContainsKey($_.Name) } | Sort-Object Name)) {
            [void]$modules.Add($m)
        }
    }
    $text = New-Object System.Text.StringBuilder
    [void]$text.Append($apiPrologue)
    foreach ($module in $modules) {
        [void]$text.Append("// ---- $($module.Name) ----`n")
        [void]$text.Append((Read-Utf8 -Path $module.FullName))
        [void]$text.Append("`n")
    }
    [void]$text.Append($apiEpilogue)
    return $text.ToString()
}

$apiText = Get-ApiText -Dir $ApiDir
$nodeText = if (Test-Path $NodeJs) { Read-Utf8 -Path $NodeJs } else { '' }

$out = New-Object 'System.Collections.Generic.List[string]'
$out.Add('// Generated by tools/embed-js.ps1 -- do not edit. Edit js\api\*.js / js\nw_node.js instead.')
$out.Add('#include "nw_script.h"')
$out.Add('')
$out.Add('namespace nmb {')
$out.Add('namespace nw {')
$out.Add('')
$out.AddRange([string[]](Emit-Function -Text $apiText -Symbol 'BuiltinApiScript'))
$out.Add('')
$out.AddRange([string[]](Emit-Function -Text $nodeText -Symbol 'BuiltinNodeScript'))
$out.Add('')
$out.Add('} // namespace nw')
$out.Add('} // namespace nmb')
$out.Add('')

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($Output, ($out.ToArray() -join "`r`n"), $utf8NoBom)
Write-Host "[embed-js] wrote $Output (api $($apiText.Length) chars / node $($nodeText.Length) chars)"
