// Runs before TombEngine starts in the browser.
//
// Games are made on Windows, where file names don't care about case and paths
// may use backslashes, and games rely on both (a script loading "Data\Title.ten"
// from data/title.ten). The engine's file system is made to behave the same.

Module['preRun'] = Module['preRun'] || [];
Module['preRun'].push(function () {
  var ENOENT = 44;

  var lookupPath = FS.lookupPath;
  FS.lookupPath = function (path, options) {
    if (typeof path === 'string' && path.indexOf('\\') >= 0) {
      path = path.replace(/\\/g, '/');
    }
    return lookupPath(path, options);
  };

  var lookupNode = FS.lookupNode;
  FS.lookupNode = function (parent, name) {
    try {
      return lookupNode(parent, name);
    } catch (error) {
      if (!(error instanceof FS.ErrnoError) || error.errno !== ENOENT || !parent.contents) {
        throw error;
      }
      var wanted = name.toLowerCase();
      for (var key in parent.contents) {
        if (key.toLowerCase() === wanted) {
          return lookupNode(parent, key);
        }
      }
      throw error;
    }
  };
});
