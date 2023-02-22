#import "WhisperCordova.h"
#import <Cordova/CDV.h>
#import <Photos/Photos.h>

@implementation SaveImage
@synthesize callbackId;

- (void)saveImageToGallery:(CDVInvokedUrlCommand*)command {
	[self.commandDelegate runInBackground:^{
	    self.callbackId = command.callbackId;

		UIImage *image = nil;
		NSString *imgAbsolutePath = [command.arguments objectAtIndex:0];
		BOOL *isBase64 = [command.arguments objectAtIndex:1];
        if ([isBase64 boolValue] == YES) {
            image = [self decodeBase64ToImage: imgAbsolutePath];
        } else {
            image = [UIImage imageWithContentsOfFile:imgAbsolutePath];
        }
        NSLog(@"Image absolute path: %@", imgAbsolutePath);

	    UIImageWriteToSavedPhotosAlbum(image, self, @selector(image:didFinishSavingWithError:contextInfo:), nil);
	}];
}

- (UIImage *)decodeBase64ToImage:(NSString *)strEncodeData {
    NSData *data = [[NSData alloc]initWithBase64EncodedString:strEncodeData options:NSDataBase64DecodingIgnoreUnknownCharacters];
    return [UIImage imageWithData:data];
}

- (void)dealloc {
	[callbackId release];
    [super dealloc];
}

- (void)image:(UIImage *)image didFinishSavingWithError:(NSError *)error contextInfo:(void *)contextInfo {
    // Was there an error?
    if (error != NULL) {
        NSLog(@"SaveImage, error: %@",error);
		CDVPluginResult* result = [CDVPluginResult resultWithStatus: CDVCommandStatus_ERROR messageAsString:error.description];
		[self.commandDelegate sendPluginResult:result callbackId:self.callbackId];
    } else {
        // No errors

        // Show message image successfully saved
        NSLog(@"SaveImage, image saved");
		CDVPluginResult* result = [CDVPluginResult resultWithStatus: CDVCommandStatus_OK messageAsString:@"Image saved"];
		[self.commandDelegate sendPluginResult:result callbackId:self.callbackId];
    }
}

@end
