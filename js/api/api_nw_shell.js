// api_nw_shell.js —— nw.Shell。对应 SDK：src/resources/api_nw_shell.js。

  function Shell() {}
  Shell.prototype.openExternal = function (uri) { return rpc('shell.openExternal', { target: uri }); };
  Shell.prototype.openItem = function (path) { return rpc('shell.openItem', { target: path }); };
  Shell.prototype.showItemInFolder = function (path) { return rpc('shell.showItemInFolder', { target: path }); };
  Shell.prototype.beep = function () { return rpc('shell.beep'); };
  exposeStatics(Shell);
