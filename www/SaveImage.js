var ImageSaver = function () {
};

ImageSaver.saveImageToGallery = function (localImagePath, isBase64, successCallback, failureCallback) {
    if (typeof successCallback != 'function') {
        throw new Error('SaveImage Error: successCallback is not a function');
    }

    if (typeof failureCallback != 'function') {
        throw new Error('SaveImage Error: failureCallback is not a function');
    }
    if(isBase64 === true){
    localImagePath = localImagePath.split(',')[1];
	}

    return cordova.exec(
        successCallback, failureCallback, 'WhisperCordova', 'saveImageToGallery', [_getLocalImagePathWithoutPrefix(), isBase64]);

    function _getLocalImagePathWithoutPrefix() {
        if (localImagePath.indexOf('file:///') === 0) {
            return localImagePath.substring(7);
        }
        return localImagePath;
    }
};

module.exports = ImageSaver;
