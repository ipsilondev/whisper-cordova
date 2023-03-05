cordova.define("whisper-cordova.WhisperCordova", function(require, exports, module) {
var WhisperCordova = function () {
};

WhisperCordova.DecodeChunkAudio = function (localImagePath, isBase64, fromTime, successCallback, failureCallback) {
    if (typeof successCallback != 'function') {
        throw new Error('DecodeChunkAudio Error: successCallback is not a function');
    }

    if (typeof failureCallback != 'function') {
        throw new Error('DecodeChunkAudio Error: failureCallback is not a function');
    }
    if(isBase64 === true){
    localImagePath = localImagePath.split(',')[1];
	   }

    return cordova.exec(
        successCallback, failureCallback, 'WhisperCordova', 'decodeChunkAudio', [_getLocalImagePathWithoutPrefix(), isBase64, fromTime]);

    function _getLocalImagePathWithoutPrefix() {
        if (localImagePath.indexOf('file:///') === 0) {
            return localImagePath.substring(7);
        }
        return localImagePath;
    }
};

module.exports = WhisperCordova;

});
