package com.ipsilondev.whispercordova;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.nio.channels.FileChannel;

import java.text.SimpleDateFormat;
import java.util.Date;

import org.apache.cordova.CallbackContext;
import org.apache.cordova.CordovaPlugin;
import org.apache.cordova.PermissionHelper;
import org.json.JSONArray;
import org.json.JSONException;

import android.Manifest;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.net.Uri;
import android.os.Environment;
import android.util.Base64;
import android.util.Log;

/**
 * The SaveImage class offers a method saving an image to the devices' media gallery.
 */
public class WhisperCordova extends CordovaPlugin {
    public static final int WRITE_PERM_REQUEST_CODE = 1;
    private final String ACTION = "decodeChunkAudio";
    private final String WRITE_EXTERNAL_STORAGE = Manifest.permission.WRITE_EXTERNAL_STORAGE;
    private CallbackContext callbackContext;
    private String filePath;
    private int isBase64 = 0;
    private float fromTime = 0;

static {
  System.loadLibrary("native-lib");
}

    @Override
    public boolean execute(String action, JSONArray args, CallbackContext callbackContext) throws JSONException {
        if (action.equals(ACTION)) {
            decodeChunkAudio(args, callbackContext);
            return true;
        } else {
            return false;
        }
    }

    // Load model by TF Lite C++ API
        private native String loadModelJNI(AssetManager assetManager, String fileName, int isBase64, float fromTime);
        private native int  freeModelJNI();
        @Override
            public void onDestroy() {
                super.onDestroy();
                freeModelJNI();
            }
    /**
     * Check saveImage arguments and app permissions
     *
     * @param args              JSON Array of args
     * @param callbackContext   callback id for optional progress reports
     *
     * args[0] filePath         file path string to image file to be saved to gallery
     */
    private void decodeChunkAudio(JSONArray args, CallbackContext callback) throws JSONException {
    	this.filePath = args.getString(0);
      this.isBase64 = args.getInt(1);
      this.fromTime = Float.parseFloat(args.getString(2));
    	this.callbackContext = callback;
        Log.d("DecodeChunkAudio", "DecodeChunkAudio in filePath: " + filePath);

        if (filePath == null || filePath.equals("")) {
        	callback.error("Missing filePath variable");
            return;
        }

        if (PermissionHelper.hasPermission(this, WRITE_EXTERNAL_STORAGE)) {
        	Log.d("whispercordova", "Permissions already granted, or Android version is lower than 6");
        	loadModelJNI(this.cordova.getActivity().getApplicationContext().getAssets(), this.filePath, this.isBase64, this.fromTime);
        } else {
        	Log.d("whispercordova", "Requesting permissions for WRITE_EXTERNAL_STORAGE");
        	PermissionHelper.requestPermission(this, WRITE_PERM_REQUEST_CODE, WRITE_EXTERNAL_STORAGE);
        }
    }

    /**
     * Callback from PermissionHelper.requestPermission method
     */
	public void onRequestPermissionResult(int requestCode, String[] permissions, int[] grantResults) throws JSONException {
		for (int r : grantResults) {
			if (r == PackageManager.PERMISSION_DENIED) {
				Log.d("SaveImage", "Permission not granted by the user");
				callbackContext.error("Permissions denied");
				return;
			}
		}

		switch (requestCode) {
		case WRITE_PERM_REQUEST_CODE:
			Log.d("SaveImage", "User granted the permission for WRITE_EXTERNAL_STORAGE");
			loadModelJNI(this.cordova.getActivity().getApplicationContext().getAssets(), this.filePath, this.isBase64, this.fromTime);
			break;
		}
	}
}
