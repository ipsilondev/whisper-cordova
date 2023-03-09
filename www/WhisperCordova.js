var exec = require("cordova/exec");
var PLUGIN_NAME = "WhisperCordova";

module.exports = {
DecodeChunkAudio: function (localImagePath, isBase64, fromTime, successCallback, failureCallback) {
    if (typeof successCallback != 'function') {
        throw new Error('DecodeChunkAudio Error: successCallback is not a function');
    }

    if (typeof failureCallback != 'function') {
        throw new Error('DecodeChunkAudio Error: failureCallback is not a function');
    }
    if(isBase64 === true){
    localImagePath = localImagePath.split(',')[1];
	   }

    return exec(
        successCallback, failureCallback, 'WhisperCordova', 'decodeChunkAudio', [this._getLocalImagePathWithoutPrefix(localImagePath), isBase64, fromTime]);
    },
     _getLocalImagePathWithoutPrefix: function(localImagePath) {
        if (localImagePath.indexOf('file:///') === 0) {
            return localImagePath.substring(7);
        }
        return localImagePath;
    }
};
