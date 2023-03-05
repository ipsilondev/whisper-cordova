import type { Context } from 'cordova-ts-hook'
const fs = require('fs');


async function setupModelAndLibsAndroid(ctx) {
      ctx.opts
      copyFolderRecursiveSync(__dirname.replace("scripts","") + "android/cpp", ctx.opts + "android/app");
      copyFolderRecursiveSync(__dirname.replace("scripts","") + "model", ctx.opts + "android/app");
}

async function setupModelAndLibsiOS(ctx) {

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

export = async (context: Context) => {
  if (ctx.opts.cordova.platforms.includes('ios')) {
    await setupModelAndLibsiOS(context)
  } else {
    await setupModelAndLibsAndroid(context)
  }
}
