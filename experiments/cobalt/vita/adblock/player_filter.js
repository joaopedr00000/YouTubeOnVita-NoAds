// Experimental document-start filter; original implementation for YouTubeOnVita-NoAds.
(function installAdFilter(root) {
  "use strict";
  var loc = root.location;
  if (!loc || loc.protocol !== "https:" || loc.hostname !== "www.youtube.com" ||
      !/^\/tv(?:\/|$)/.test(loc.pathname) || root.__ytvAdBlockInstalled) return false;
  var keys = ["adPlacements", "playerAds", "adSlots"];
  var own = Object.prototype.hasOwnProperty;
  function cleanPlayer(p) {
    if (!p || typeof p !== "object" || !p.playabilityStatus ||
        (!p.videoDetails && !p.streamingData)) return;
    for (var i = 0; i < keys.length; i++) {
      if (own.call(p, keys[i])) delete p[keys[i]];
    }
  }
  function clean(v) {
    try {
      if (Array.isArray(v)) {
        for (var i = 0; i < v.length && i < 32; i++) {
          cleanPlayer(v[i]);
          if (v[i] && v[i].playerResponse) cleanPlayer(v[i].playerResponse);
        }
      } else {
        cleanPlayer(v);
        if (v && v.playerResponse) cleanPlayer(v.playerResponse);
      }
    } catch (_) { /* fail open on unexpected or immutable objects */ }
    return v;
  }
  var parse = root.JSON.parse;
  root.JSON.parse = function () { return clean(parse.apply(this, arguments)); };
  if (root.Response && root.Response.prototype &&
      typeof root.Response.prototype.json === "function") {
    try {
      var json = root.Response.prototype.json;
      root.Response.prototype.json = function () {
        return json.apply(this, arguments).then(clean);
      };
    } catch (_) {}
  }
  if (root.XMLHttpRequest && root.XMLHttpRequest.prototype) {
    try {
      var proto = root.XMLHttpRequest.prototype;
      var desc = Object.getOwnPropertyDescriptor(proto, "response");
      if (desc && desc.configurable && typeof desc.get === "function") {
        var get = desc.get;
        desc.get = function () {
          var value = get.call(this);
          return this.responseType === "json" ? clean(value) : value;
        };
        Object.defineProperty(proto, "response", desc);
      }
    } catch (_) {}
  }
  clean(root.ytInitialPlayerResponse);
  Object.defineProperty(root, "__ytvAdBlockInstalled", {value: true});
  return true;
})(window);
