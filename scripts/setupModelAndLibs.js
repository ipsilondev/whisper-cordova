const fs = require('fs');


function setupModelAndLibsAndroid(ctx) {
      copyFolderRecursiveSync(__dirname.replace("scripts","") + "android/cpp", ctx.opts + "android/app");
      copyFolderRecursiveSync(__dirname.replace("scripts","") + "model", ctx.opts + "android/app");
}

function setupModelAndLibsiOS(ctx) {

}

function copyFolderRecursiveSync( source, target ) {
    var files = [];

    // Check if folder needs to be created or integrated
    var targetFolder = path.join( target, path.basename( source ) );
    if ( !fs.existsSync( targetFolder ) ) {
        fs.mkdirSync( targetFolder );
    }

    // Copy
    if ( fs.lstatSync( source ).isDirectory() ) {
        files = fs.readdirSync( source );
        files.forEach( function ( file ) {
            var curSource = path.join( source, file );
            if ( fs.lstatSync( curSource ).isDirectory() ) {
                copyFolderRecursiveSync( curSource, targetFolder );
            } else {
                copyFileSync( curSource, targetFolder );
            }
        } );
    }
}

module.exports = function (context) => {
  if (context.opts.cordova.platforms.includes('ios')) {
    setupModelAndLibsiOS(context)
  } else {
    setupModelAndLibsAndroid(context)
  }
}
