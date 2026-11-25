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

// Videos (see Specific/Platform/Web/VideoWeb.cpp), played by the browser's own <video>
// elements: they decode in the browser and play their sound through it, and the
// engine copies their frames into a texture on the GPU. The page can say where a
// video is with Module.tenWebVideoUrl(path), so large videos stream from there;
// otherwise a video found in the engine's file system plays from memory.
// Main thread only, as the elements; the engine calls these through it.
Module['tenWebVideo'] = {
  players: {},

  // Opens the first of the paths found. Returns its index, or -1.
  open: function (id, paths, loop, muted) {
    for (var i = 0; i < paths.length; i++) {
      var url = Module['tenWebVideoUrl'] ? Module['tenWebVideoUrl'](paths[i]) : null;
      var objectUrl = null;
      if (!url) {
        try {
          var data = FS.readFile(paths[i]);
          objectUrl = url = URL.createObjectURL(new Blob([data]));
        } catch (error) {
          continue;
        }
      }

      // In the page but not seen: some browsers don't decode videos outside it.
      var video = document.createElement('video');
      video.playsInline = true;
      video.preload = 'auto';
      video.loop = !!loop;
      video.muted = !!muted;
      video.style.cssText = 'position:fixed;left:0;top:0;width:1px;height:1px;opacity:0;pointer-events:none';
      document.body.appendChild(video);

      var player = { video: video, objectUrl: objectUrl, frames: 0, lastTime: -1, error: false };
      video.addEventListener('error', function () { player.error = true; });
      video.src = url;
      this.players[id] = player;
      this.play(id);
      return i;
    }
    return -1;
  },

  play: function (id) {
    var player = this.players[id];
    if (!player)
      return;

    var started = player.video.play();
    if (started && started.catch) {
      started.catch(function (error) {
        if (error && error.name === 'NotAllowedError' && !player.video.muted) {
          // Not allowed to play with sound yet: play without.
          player.video.muted = true;
          player.video.play().catch(function () { player.error = true; });
        } else if (error && error.name !== 'AbortError') {
          player.error = true;
        }
      });
    }
  },

  pause: function (id) {
    var player = this.players[id];
    if (player)
      player.video.pause();
  },

  seek: function (id, seconds) {
    var player = this.players[id];
    if (player)
      player.video.currentTime = seconds;
  },

  // Writes the frames shown so far, time, duration, width, height and flags
  // (1 ready, 2 playing, 4 paused, 8 ended, 16 failed) as doubles at the address.
  state: function (id, address, volume) {
    var player = this.players[id];
    var values = [0, 0, 0, 0, 0, 16];
    if (player) {
      var video = player.video;
      video.volume = Math.min(Math.max(volume, 0), 1);
      var ready = video.readyState >= 2 && video.videoWidth > 0;
      if (ready && video.currentTime !== player.lastTime) {
        player.lastTime = video.currentTime;
        player.frames++;
      }
      var duration = isFinite(video.duration) ? video.duration : 0;
      values = [player.frames, video.currentTime, duration, video.videoWidth, video.videoHeight,
        (ready ? 1 : 0) | (!video.paused && !video.ended ? 2 : 0) | (video.paused ? 4 : 0) |
        (video.ended ? 8 : 0) | (player.error ? 16 : 0)];
    }
    for (var i = 0; i < values.length; i++)
      HEAPF64[(address >> 3) + i] = values[i];
  },

  // Into the texture bound to the context, of the given size: the video's own, or
  // smaller for large videos, which a canvas scales down first.
  upload: function (id, gl, width, height) {
    var player = this.players[id];
    if (!player || player.video.readyState < 2 || player.video.videoWidth === 0)
      return;

    var video = player.video;
    if (video.videoWidth === width && video.videoHeight === height) {
      gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, gl.RGBA, gl.UNSIGNED_BYTE, video);
      return;
    }

    if (!player.canvas || player.canvas.width !== width || player.canvas.height !== height) {
      player.canvas = document.createElement('canvas');
      player.canvas.width = width;
      player.canvas.height = height;
      player.context = player.canvas.getContext('2d');
    }
    player.context.drawImage(video, 0, 0, width, height);
    gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, gl.RGBA, gl.UNSIGNED_BYTE, player.canvas);
  },

  // The average colour of the frame shown, as 0xRRGGBB.
  averageColor: function (id) {
    var player = this.players[id];
    if (!player || player.video.readyState < 2)
      return 0;

    if (!this.canvas) {
      this.canvas = document.createElement('canvas');
      this.canvas.width = this.canvas.height = 8;
      this.context = this.canvas.getContext('2d', { willReadFrequently: true });
    }
    this.context.drawImage(player.video, 0, 0, 8, 8);
    var pixels = this.context.getImageData(0, 0, 8, 8).data;
    var r = 0, g = 0, b = 0;
    for (var i = 0; i < pixels.length; i += 4) {
      r += pixels[i]; g += pixels[i + 1]; b += pixels[i + 2];
    }
    return ((Math.round(r / 64) << 16) | (Math.round(g / 64) << 8) | Math.round(b / 64)) >>> 0;
  },

  close: function (id) {
    var player = this.players[id];
    if (!player)
      return;

    player.video.pause();
    player.video.removeAttribute('src');
    player.video.load();
    player.video.remove();
    if (player.objectUrl)
      URL.revokeObjectURL(player.objectUrl);
    delete this.players[id];
  },
};
