package com.bilko.forsakenvr;

import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;

import org.libsdl.app.SDLActivity;


/**
 * Forsaken VR entry point. SDL provides the Android lifecycle, input, audio and the GL ES
 * context; the game itself is libmain.so. Game data is read from /sdcard/Forsaken, which
 * needs "All files access". Files packed in the APK are installed there by fx_android.c.
 */
public class ForsakenActivity extends SDLActivity
{
    @Override
    protected void onCreate( Bundle savedInstanceState )
    {
        super.onCreate( savedInstanceState );
        RequestStorageAccess();
    }

    private boolean HasStorageAccess()
    {
        return Build.VERSION.SDK_INT < Build.VERSION_CODES.R || Environment.isExternalStorageManager();
    }

    private void RequestStorageAccess()
    {
        if( ! HasStorageAccess() )
        {
            try
            {
                Intent intent = new Intent( Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION );
                intent.setData( Uri.parse( "package:" + getPackageName() ) );
                startActivity( intent );
            }
            catch( Exception e )
            {
                startActivity( new Intent( Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION ) );
            }
        }
    }

    @Override
    protected void onDestroy()
    {
        // The game keeps threads (audio, network) that would outlive the activity. End the whole
        // process once the activity is gone; watchdog in case the game thread doesn't stop.
        Thread watchdog = new Thread( new Runnable()
        {
            @Override
            public void run()
            {
                try { Thread.sleep( 3000 ); } catch( InterruptedException e ) { return; }
                android.os.Process.killProcess( android.os.Process.myPid() );
            }
        } );
        watchdog.setDaemon( true );
        watchdog.start();
        super.onDestroy();
        android.os.Process.killProcess( android.os.Process.myPid() );
    }

    @Override
    protected String[] getArguments()
    {
        // Developer testing: adb shell am start ... --es args "-QuickStart"
        String args = getIntent() != null ? getIntent().getStringExtra( "args" ) : null;
        if( args == null || args.trim().isEmpty() )
            return new String[0];
        return args.trim().split( "\\s+" );
    }

    @Override
    protected String[] getLibraries()
    {
        return new String[] { "SDL2", "main" };
    }
}
