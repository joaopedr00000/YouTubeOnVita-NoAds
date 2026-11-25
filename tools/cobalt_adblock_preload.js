/* YouTubeOnVita-NoAds: original schema filter inspired by the approach
 * documented by TizenTube and webOS Cobalt AdFree. Requires an early Cobalt
 * preload; copying this file into a VPK alone does not execute it.
 * No network interception, certificates, account data, or remote scripts.
 */
(function () {
  'use strict';
  if (typeof JSON.parse !== 'function' || JSON.parse.__ytvNoAds) return;
  var nativeParse = JSON.parse;
  var adFields = ['adPlacements', 'playerAds', 'adSlots'];
  var renderers = ['adSlotRenderer', 'promotedSparklesWebRenderer',
    'promotedVideoRenderer', 'displayAdRenderer', 'inFeedAdLayoutRenderer'];
  var own = Object.prototype.hasOwnProperty;
  function isAd(entry) {
    if (!entry || typeof entry !== 'object' || Array.isArray(entry)) return false;
    // Remove only a recognized renderer wrapper, not an object containing
    // ordinary content alongside ad metadata.
    var keys = Object.keys(entry);
    if (keys.length === 1 && renderers.indexOf(keys[0]) !== -1) return true;
    var endpoint = entry.command && entry.command.reelWatchEndpoint;
    return !!(endpoint && endpoint.adClientParams &&
      endpoint.adClientParams.isAd === true);
  }
  function filter(root) {
    if (!root || typeof root !== 'object') return root;
    // Avoid modifying unrelated JSON parsed by the application.
    if (!own.call(root, 'responseContext') && !own.call(root, 'playabilityStatus') &&
        !adFields.some(function (k) { return own.call(root, k); })) return root;
    var stack = [root], visited = 0;
    while (stack.length && visited++ < 40000) {
      var value = stack.pop();
      if (!value || typeof value !== 'object') continue;
      if (Array.isArray(value)) {
        // In-place stable compaction avoids allocating a second large feed.
        var write = 0;
        for (var i = 0; i < value.length; i++) {
          if (!own.call(value, i)) continue;
          var item = value[i];
          if (isAd(item)) continue;
          value[write++] = item;
          if (item && typeof item === 'object') stack.push(item);
        }
        value.length = write;
      } else {
        adFields.forEach(function (k) {
          if (own.call(value, k)) {
            // Preserve expected array/bool shape, never touch streamingData.
            if (Array.isArray(value[k])) value[k] = [];
            else if (typeof value[k] === 'boolean') value[k] = false;
          }
        });
        Object.keys(value).forEach(function (k) {
          if (adFields.indexOf(k) !== -1 || k === 'streamingData') return;
          var child = value[k];
          if (child && typeof child === 'object') stack.push(child);
        });
      }
    }
    return root;
  }
  function parse(text, reviver) {
    var result = nativeParse.apply(this, arguments);
    // A custom reviver may intentionally construct non-JSON data (cycles,
    // getters, sparse arrays, etc.). Do not modify that caller's result.
    if (typeof reviver === 'function') return result;
    try { return filter(result); } catch (_) { return result; }
  }
  Object.defineProperty(parse, '__ytvNoAds', { value: true });
  JSON.parse = parse;
}());
