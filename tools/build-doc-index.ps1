﻿﻿﻿﻿﻿# build-doc-index.ps1 -- bundle every mb-docs/*.md into one offline index.html.
#
# Why a generator (instead of fetch() at runtime): over file:// most browsers
# block XHR/fetch of sibling files, so a static viewer has to carry the documents
# inside itself. Re-run this script whenever a doc under mb-docs is added or
# changed. Output: mb-docs\index.html (UTF-8, no server / no network needed).
#
# Encoding: this file MUST stay UTF-8 *with BOM* -- the HTML template below carries
# Chinese UI literals, and PowerShell 5.1 on a zh-CN box reads a BOM-less .ps1 as
# GBK (mojibake). The BOM makes both PS5.1 and PS7 parse it as UTF-8.
# (The other scripts, embed-js.ps1 / *.bat, stay ASCII-only by convention.)
param(
    [string]$DocsDir = (Join-Path $PSScriptRoot '..\mb-docs')
)

$ErrorActionPreference = 'Stop'
$DocsDir = (Resolve-Path -LiteralPath $DocsDir).Path

# Group order + labels (labels are English here; the page UI itself is Chinese).
$groups = @(
    @{ dir = '';         label = 'Start';         files = @('mb.md') },
    # The other root doc is the Chinese-named 00-*.md overview; picked up by the
    # stray-file safety net below (group label is normalized there for root files).
    @{ dir = 'src';      label = 'native (C++/JS core)' },
    @{ dir = 'api';      label = 'page JS (js/api)' },
    @{ dir = 'build';    label = 'build & tools' },
    @{ dir = 'tests';    label = 'tests' },
    @{ dir = 'reference'; label = 'upstream reference' }
)

function Get-Title([string]$text, [string]$fallback) {
    foreach ($line in ($text -split "`r?`n")) {
        if ($line -match '^\s*#\s+(.+?)\s*$') { return $Matches[1].Trim() }
        if ($line.Trim().Length -gt 0) { break }
    }
    return $fallback
}

$items = New-Object 'System.Collections.Generic.List[object]'
$seen = @{}

foreach ($g in $groups) {
    if ($g.dir -eq '') {
        foreach ($fn in $g.files) {
            $path = Join-Path $DocsDir $fn
            if (-not (Test-Path -LiteralPath $path)) { continue }
            $text = [System.IO.File]::ReadAllText($path, [System.Text.Encoding]::UTF8)
            $items.Add([pscustomobject]@{ p = $fn; g = $g.label; t = Get-Title $text $fn; m = $text })
            $seen[$fn] = $true
        }
        continue
    }
    $gdir = Join-Path $DocsDir $g.dir
    if (-not (Test-Path -LiteralPath $gdir)) { continue }
    foreach ($fi in (Get-ChildItem -LiteralPath $gdir -Filter *.md | Sort-Object Name)) {
        $rel = ($g.dir + '/' + $fi.Name)
        $text = [System.IO.File]::ReadAllText($fi.FullName, [System.Text.Encoding]::UTF8)
        $items.Add([pscustomobject]@{ p = $rel; g = $g.label; t = Get-Title $text $fi.BaseName; m = $text })
        $seen[$rel] = $true
    }
}

# Pick up any stray .md not covered above (safety net, keeps the index complete).
Get-ChildItem -LiteralPath $DocsDir -Recurse -Filter *.md | Sort-Object FullName | ForEach-Object {
    $rel = $_.FullName.Substring($DocsDir.Length + 1).Replace('\', '/')
    if (-not $seen.ContainsKey($rel)) {
        $text = [System.IO.File]::ReadAllText($_.FullName, [System.Text.Encoding]::UTF8)
        $grp = if ($rel -notmatch '/') { 'Start' } else { 'other' }
        $items.Add([pscustomobject]@{ p = $rel; g = $grp; t = Get-Title $text $_.BaseName; m = $text })
    }
}

$json = ($items | ConvertTo-Json -Depth 5 -Compress)
if ($items.Count -eq 1) { $json = '[' + $json + ']' }

$template = @'
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>mb 技术文档</title>
<style>
:root{
  --bg:#f6f8fa; --panel:#ffffff; --ink:#1f2328; --muted:#656d76; --line:#d8dee4;
  --accent:#0969da; --accent-soft:#ddf4ff; --code-bg:#eff1f3; --hover:#f3f4f6; --sidebar-w:330px;
}
@media (prefers-color-scheme: dark){
  :root{ --bg:#0d1117; --panel:#161b22; --ink:#e6edf3; --muted:#9198a1; --line:#30363d;
    --accent:#58a6ff; --accent-soft:#12233f; --code-bg:#21262d; --hover:#1c2128; }
}
*{box-sizing:border-box}
html,body{margin:0;height:100%}
body{background:var(--bg);color:var(--ink);
  font-family:-apple-system,"Segoe UI","PingFang SC","Microsoft YaHei","Noto Sans CJK SC",sans-serif;
  font-size:15px;line-height:1.7}
#layout{display:flex;height:100vh}
#side{width:var(--sidebar-w);flex:0 0 var(--sidebar-w);background:var(--panel);
  border-right:1px solid var(--line);display:flex;flex-direction:column;min-width:0}
#brand{padding:16px 18px 10px;border-bottom:1px solid var(--line)}
#brand h1{margin:0;font-size:17px}
#brand .sub{color:var(--muted);font-size:12px;margin-top:2px}
#search{margin:12px 14px 8px}
#q{width:100%;padding:8px 12px;border:1px solid var(--line);border-radius:8px;
  background:var(--bg);color:var(--ink);font-size:14px;outline:none}
#q:focus{border-color:var(--accent);box-shadow:0 0 0 3px var(--accent-soft)}
#nav{overflow:auto;padding:4px 8px 24px;flex:1}
.grouph{padding:12px 10px 4px;font-size:11px;letter-spacing:.08em;
  text-transform:uppercase;color:var(--muted);font-weight:700}
.item{display:block;padding:6px 10px;border-radius:8px;cursor:pointer;
  color:var(--ink);text-decoration:none;font-size:13.5px;white-space:nowrap;
  overflow:hidden;text-overflow:ellipsis}
.item:hover{background:var(--hover)}
.item.active{background:var(--accent-soft);color:var(--accent);font-weight:600}
.item .path{display:block;color:var(--muted);font-size:11px;font-weight:400}
.item.active .path{color:var(--accent)}
.item.hidden{display:none}
#main{flex:1;overflow:auto;min-width:0}
#doc{max-width:880px;margin:0 auto;padding:36px 48px 120px}
#doc h1{font-size:28px;border-bottom:1px solid var(--line);padding-bottom:.3em;margin-top:0}
#doc h2{font-size:22px;border-bottom:1px solid var(--line);padding-bottom:.3em;margin-top:1.8em}
#doc h3{font-size:17px;margin-top:1.6em}
#doc h4{font-size:15px;margin:1.2em 0 .4em}
#doc a{color:var(--accent);text-decoration:none}
#doc a:hover{text-decoration:underline}
#doc code{font-family:"Cascadia Code",Consolas,"SFMono-Regular",Menlo,monospace;
  font-size:13px;background:var(--code-bg);padding:.15em .4em;border-radius:5px}
#doc pre{background:var(--code-bg);padding:14px 16px;border-radius:10px;overflow:auto;
  border:1px solid var(--line)}
#doc pre code{background:none;padding:0;font-size:13px;line-height:1.55}
#doc blockquote{margin:1em 0;padding:0 1em;color:var(--muted);border-left:4px solid var(--line)}
#doc table{border-collapse:collapse;margin:1em 0;display:block;overflow:auto;max-width:100%}
#doc th,#doc td{border:1px solid var(--line);padding:6px 13px;font-size:13.5px;vertical-align:top}
#doc th{background:var(--hover);font-weight:600}
#doc hr{border:none;border-top:1px solid var(--line);margin:2em 0}
#doc ul,#doc ol{padding-left:1.8em}
#doc li{margin:.25em 0}
#doc img{max-width:100%}
#topbar{position:sticky;top:0;background:var(--bg);padding:10px 0 0;z-index:2;
  display:flex;gap:8px;align-items:center}
#crumb{font-size:12.5px;color:var(--muted);margin-bottom:10px}
.pill{display:inline-block;font-size:11px;padding:1px 8px;border-radius:999px;
  background:var(--accent-soft);color:var(--accent);margin-left:6px}
@media (max-width:820px){
  #side{position:fixed;z-index:10;height:100%;left:0;top:0;transform:translateX(-100%);
    transition:transform .15s ease;box-shadow:4px 0 20px rgba(0,0,0,.2)}
  #side.open{transform:none}
  #menuBtn{display:inline-block!important}
  #doc{padding:20px 18px 80px}
}
#menuBtn{display:none;border:1px solid var(--line);background:var(--panel);color:var(--ink);
  border-radius:8px;padding:6px 12px;cursor:pointer;font-size:14px}
</style>
</head>
<body>
<div id="layout">
  <nav id="side">
    <div id="brand">
      <h1>mb 技术文档</h1>
      <div class="sub">nw 改版 · mb108 宿主 · 逐文件文档</div>
    </div>
    <div id="search"><input id="q" type="search" placeholder="搜索文档标题或全文…" autofocus></div>
    <div id="nav"></div>
  </nav>
  <main id="main">
    <div id="doc">
      <div id="topbar"><button id="menuBtn">目录</button></div>
      <div id="crumb"></div>
      <div id="content"></div>
    </div>
  </main>
</div>
<script>
"use strict";
var DOCS = __DOC_DATA__;

function esc(s){
  return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
}
function norm(p, base){
  if(!p) return p;
  if(/^[a-z]+:/i.test(p) || p.charAt(0)==='#') return p;
  var baseDir = base.indexOf('/')>=0 ? base.split('/').slice(0,-1).join('/') : '';
  var stack = baseDir ? baseDir.split('/') : [];
  p.split('/').forEach(function(part){
    if(part===''||part==='.') return;
    if(part==='..') stack.pop(); else stack.push(part);
  });
  return stack.join('/');
}
function inline(s, base){
  s = esc(s);
  s = s.replace(/`([^`]+)`/g, function(_,c){ return '<code>'+c+'</code>'; });
  s = s.replace(/!\[([^\]]*)\]\(([^)\s]+)\)/g, function(_,alt,url){
    return '<img alt="'+alt+'" src="'+esc(url)+'">';
  });
  s = s.replace(/\[([^\]]+)\]\(([^)\s]+)(?:\s+"[^"]*")?\)/g, function(_,text,url){
    var internal = !/^[a-z]+:/i.test(url) && url.charAt(0)!=='#' && url.indexOf('.md')>=0;
    var np = norm(url, base);
    if(internal){
      var hash = '';
      var h = url.indexOf('#');
      if(h>=0){ hash = url.slice(h+1); np = norm(url.slice(0,h), base); }
      return '<a href="#" data-doc="'+esc(np)+(hash?('#'+esc(hash)):'')+'">'+text+'</a>';
    }
    return '<a href="'+esc(np)+'" target="_blank" rel="noopener">'+text+'</a>';
  });
  s = s.replace(/\*\*([^*]+)\*\*/g,'<strong>$1</strong>');
  return s;
}
function renderMd(md, base){
  var lines = md.replace(/\r\n/g,'\n').split('\n');
  var html = [];
  var i = 0;
  while(i < lines.length){
    var line = lines[i];
    if(/^```/.test(line.trim())){
      var buf = [];
      i++;
      while(i < lines.length && !/^```/.test(lines[i].trim())){ buf.push(lines[i]); i++; }
      i++;
      html.push('<pre><code>'+esc(buf.join('\n'))+'</code></pre>');
      continue;
    }
    if(/^\|.+\|\s*$/.test(line)){
      var tbl=[];
      while(i < lines.length && /^\|.+\|\s*$/.test(lines[i])){ tbl.push(lines[i]); i++; }
      function cells(row){
        return row.replace(/^\||\|\s*$/g,'').split('|').map(function(c){return c.trim();});
      }
      var head = cells(tbl[0]);
      var h = '<tr>'+head.map(function(c){return '<th>'+inline(c,base)+'</th>';}).join('')+'</tr>';
      var bodyRows = tbl.slice(1).filter(function(r){ return !/^\|[\s:|-]+\|\s*$/.test(r); });
      var b = bodyRows.map(function(r){
        return '<tr>'+cells(r).map(function(c){return '<td>'+inline(c,base)+'</td>';}).join('')+'</tr>';
      }).join('');
      html.push('<table><thead>'+h+'</thead><tbody>'+b+'</tbody></table>');
      continue;
    }
    var hm = line.match(/^(#{1,6})\s+(.*)$/);
    if(hm){ html.push('<h'+hm[1].length+'>'+inline(hm[2],base)+'</h'+hm[1].length+'>'); i++; continue; }
    if(/^\s*$/.test(line)){ i++; continue; }
    if(/^\s*([-*_])\1{2,}\s*$/.test(line)){ html.push('<hr>'); i++; continue; }
    if(/^&gt;\s?/.test(line) || /^>\s?/.test(line)){
      var q=[];
      while(i<lines.length && /^>\s?/.test(lines[i])){ q.push(lines[i].replace(/^>\s?/,'')); i++; }
      html.push('<blockquote>'+inline(q.join(' '),base)+'</blockquote>');
      continue;
    }
    var lm = line.match(/^(\s*)([-*+])\s+(.*)$/);
    if(lm){
      var ul=[];
      while(i<lines.length && /^\s*[-*+]\s+/.test(lines[i])){ ul.push(lines[i].replace(/^\s*[-*+]\s+/,'')); i++; }
      html.push('<ul>'+ul.map(function(x){return '<li>'+inline(x,base)+'</li>';}).join('')+'</ul>');
      continue;
    }
    var om = line.match(/^(\s*)\d+\.\s+(.*)$/);
    if(om){
      var ol=[];
      while(i<lines.length && /^\s*\d+\.\s+/.test(lines[i])){ ol.push(lines[i].replace(/^\s*\d+\.\s+/,'')); i++; }
      html.push('<ol>'+ol.map(function(x){return '<li>'+inline(x,base)+'</li>';}).join('')+'</ol>');
      continue;
    }
    if(/^<(div|br|hr|img|a)\b/i.test(line)){ html.push(line); i++; continue; }
    var para=[];
    while(i<lines.length && lines[i].trim() && !/^(#{1,6}\s|```|\||>|\s*[-*+]\s|\s*\d+\.\s|<)/.test(lines[i])
          && !/^\s*([-*_])\1{2,}\s*$/.test(lines[i])){ para.push(lines[i]); i++; }
    html.push('<p>'+inline(para.join(' '),base)+'</p>');
  }
  return html.join('\n');
}

var byPath = {};
DOCS.forEach(function(d){ byPath[d.p] = d; });

var nav = document.getElementById('nav');
var groups = {};
DOCS.forEach(function(d){
  if(!groups[d.g]) groups[d.g] = [];
  groups[d.g].push(d);
});
Object.keys(groups).forEach(function(g){
  var h = document.createElement('div');
  h.className = 'grouph';
  h.textContent = g + ' (' + groups[g].length + ')';
  nav.appendChild(h);
  groups[g].forEach(function(d){
    var a = document.createElement('a');
    a.className = 'item';
    a.href = '#';
    a.setAttribute('data-nav', d.p);
    a.innerHTML = esc(d.t) + '<span class="path">'+esc(d.p)+'</span>';
    a.addEventListener('click', function(e){ e.preventDefault(); openDoc(d.p); });
    nav.appendChild(a);
  });
});

var content = document.getElementById('content');
var crumb = document.getElementById('crumb');
function openDoc(path, hash){
  var d = byPath[path];
  if(!d){ return; }
  document.title = d.t + ' — mb 技术文档';
  content.innerHTML = renderMd(d.m, d.p);
  crumb.innerHTML = '<span class="pill">'+esc(d.g)+'</span> ' + esc(d.p);
  var nodes = nav.querySelectorAll('[data-nav]');
  for(var k=0;k<nodes.length;k++) nodes[k].classList.toggle('active', nodes[k].getAttribute('data-nav')===path);
  document.getElementById('main').scrollTop = 0;
  if(hash){
    var target = document.getElementById(hash) ||
      content.querySelector('[id="'+hash+'"], a[name="'+hash+'"]');
    if(target) target.scrollIntoView();
  }
  if(window.innerWidth <= 820) document.getElementById('side').classList.remove('open');
}

document.addEventListener('click', function(e){
  var a = e.target.closest ? e.target.closest('a[data-doc]') : null;
  if(a){
    e.preventDefault();
    var v = a.getAttribute('data-doc');
    var hpos = v.indexOf('#');
    if(hpos>=0) openDoc(v.slice(0,hpos), v.slice(hpos+1)); else openDoc(v);
  }
});

document.getElementById('q').addEventListener('input', function(){
  var q = this.value.trim().toLowerCase();
  var items = nav.querySelectorAll('.item');
  var heads = nav.querySelectorAll('.grouph');
  items.forEach(function(it){
    var d = byPath[it.getAttribute('data-nav')];
    var hit = !q || d.t.toLowerCase().indexOf(q)>=0 || d.m.toLowerCase().indexOf(q)>=0;
    it.classList.toggle('hidden', !hit);
  });
  heads.forEach(function(h){
    var next = h.nextElementSibling, any = false;
    while(next && next.classList.contains('item')){
      if(!next.classList.contains('hidden')){ any = true; break; }
      next = next.nextElementSibling;
    }
    h.style.display = any ? '' : 'none';
  });
});

document.getElementById('menuBtn').addEventListener('click', function(){
  document.getElementById('side').classList.toggle('open');
});

var start = DOCS.some(function(d){return d.p==='mb.md';}) ? 'mb.md' : DOCS[0].p;
openDoc(start);
</script>
</body>
</html>
'@

$html = $template.Replace('__DOC_DATA__', $json)
$outPath = Join-Path $DocsDir 'index.html'
[System.IO.File]::WriteAllText($outPath, $html, (New-Object System.Text.UTF8Encoding($false)))
Write-Host ("[doc-index] bundled {0} docs -> {1}" -f $items.Count, $outPath)
