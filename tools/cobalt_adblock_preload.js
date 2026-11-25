/* Original schema filter; source-only until a compatible patched Cobalt runtime is packaged. No TLS interception or media URL blocking. */
(function install(root) {
  'use strict';
  var json=root.JSON, nativeParse=json.parse, own=Object.prototype.hasOwnProperty;
  if(typeof nativeParse!=='function'||nativeParse.__ytvNoAds)return;
  var fields=['adPlacements','playerAds','adSlots','adBreaks','adBreakParams','adPlacement','adSlot','adSignalsInfo'];
  var renderers=['adSlotRenderer','promotedSparklesWebRenderer','promotedSparklesTextSearchRenderer','promotedVideoRenderer','displayAdRenderer','inFeedAdLayoutRenderer','videoMastheadAdV3Renderer','bannerPromoRenderer','compactPromotedVideoRenderer','searchPyvRenderer','companionAdRenderer','actionCompanionAdRenderer','instreamVideoAdRenderer','adLayoutRenderer'];
  function has(o,k){return own.call(o,k)}
  function ad(o){
    if(!o||typeof o!=='object'||Array.isArray(o))return false;
    var keys=Object.keys(o);
    if(keys.length&&keys.some(function(k){return renderers.indexOf(k)!==-1;})&&keys.every(function(k){return renderers.indexOf(k)!==-1||k==='trackingParams'||k==='clickTrackingParams';}))return true;
    var e=o.command&&o.command.reelWatchEndpoint;
    if(!e)e=o.reelWatchEndpoint;
    return !!(e&&e.adClientParams&&e.adClientParams.isAd===true);
  }
  function known(o){
    return !!(o&&typeof o==='object'&&(has(o,'responseContext')||has(o,'playabilityStatus')||fields.some(function(k){return has(o,k);})||
      (has(o,'continuationContents')||has(o,'onResponseReceivedActions')||has(o,'onResponseReceivedEndpoints'))||
      (Array.isArray(o)&&o.some(function(v){return v&&typeof v==='object'&&(has(v,'responseContext')||has(v,'playabilityStatus'));}))));
  }
  function filter(o){
    if(!known(o))return o;
    var stack=[o],seen=0;
    while(stack.length&&seen++<40000){
      var v=stack.pop();
      if(!v||typeof v!=='object')continue;
      if(Array.isArray(v)){
        var w=0;
        for(var i=0;i<v.length;i++){if(!has(v,i))continue;var item=v[i];if(ad(item))continue;v[w++]=item;if(item&&typeof item==='object')stack.push(item);}
        v.length=w;
      }else{
        Object.keys(v).forEach(function(k){
          if(k==='streamingData'||k==='videoDetails'||k==='playabilityStatus'||k==='responseContext')return;
          if(fields.indexOf(k)!==-1){
            if(Array.isArray(v[k]))v[k]=[];
            else if(typeof v[k]==='boolean')v[k]=false;
            else delete v[k];
            return;
          }
          if(renderers.indexOf(k)!==-1){delete v[k];return;}
          var child=v[k];if(child&&typeof child==='object')stack.push(child);
        });
      }
    }
    return o;
  }
  function safe(o){try{return filter(o);}catch(_){return o;}}
  function parse(text,reviver){var result=nativeParse.apply(this,arguments);return typeof reviver==='function'?result:safe(result);}
  Object.defineProperty(parse,'__ytvNoAds',{value:true});
  json.parse=parse;
  function youtubeResponse(url){
    return typeof url==='string'&&/^https:\/\/(?:www\.|m\.)?youtube\.com\/(?:youtubei\/v1\/|tv(?:[/?#]|$)|watch(?:[/?#]|$))/.test(url);
  }
  // Some TV clients decode the response natively, bypassing JSON.parse.
  try{
    var p=root.Response&&root.Response.prototype;
    if(p&&typeof p.json==='function'){
      var original=p.json;
      p.json=function(){var match=youtubeResponse(this.url);var result=original.apply(this,arguments);return match?result.then(safe):result;};
    }
  }catch(_){}
  try{
    var x=root.XMLHttpRequest&&root.XMLHttpRequest.prototype;
    var d=x&&Object.getOwnPropertyDescriptor(x,'response');
    if(d&&d.configurable&&typeof d.get==='function'){
      Object.defineProperty(x,'response',{configurable:d.configurable,enumerable:d.enumerable,set:d.set,get:function(){
        var result=d.get.call(this);
        return this.responseType==='json'&&youtubeResponse(this.responseURL)?safe(result):result;
      }});
    }
  }catch(_){}
  // Initial data is sometimes assigned directly rather than parsed from JSON.
  ['ytInitialPlayerResponse','ytInitialData'].forEach(function(k){
    try{
      if(Object.getOwnPropertyDescriptor(root,k))return;
      var value;
      Object.defineProperty(root,k,{configurable:true,enumerable:true,get:function(){return value;},set:function(v){value=safe(v);}});
    }catch(_){}
  });
})(typeof window==='object'?window:globalThis);
