package com.bilko.forsakenvr;

import android.content.Intent;
import android.content.res.AssetManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * Forsaken VR entry point. SDL provides the Android lifecycle, input, audio and the GL ES
 * context; the game itself is libmain.so. Game data is read from /sdcard/Forsaken, which
 * needs "All files access".
 */
public class ForsakenActivity extends SDLActivity
{
    @Override
    protected void onCreate( Bundle savedInstanceState )
    {
        // Before SDL starts the game: put the files that ship with the APK (scripts, default
        // settings) into /sdcard/Forsaken. The game data itself is copied there by the player.
        if( HasStorageAccess() )
            InstallBundledFiles();
        super.onCreate( savedInstanceState );
        RequestStorageAccess();
    }

    private boolean HasStorageAccess()
    {
        return Build.VERSION.SDK_INT < Build.VERSION_CODES.R || Environment.isExternalStorageManager();
    }

    // scripts/ is refreshed on every version change; configs/ and pilots/ only when missing,
    // so the player's settings survive updates.
    private void InstallBundledFiles()
    {
        File root = new File( Environment.getExternalStorageDirectory(), "Forsaken" );
        root.mkdirs();
        String version = String.valueOf( BuildConfig.VERSION_CODE );
        File stamp = new File( root, "scripts/.apk-version" );
        boolean update = ! version.equals( ReadText( stamp ) );
        AssetManager assets = getAssets();
        CopyAssets( assets, "scripts", new File( root, "scripts" ), update );
        CopyAssets( assets, "configs", new File( root, "configs" ), false );
        CopyAssets( assets, "pilots", new File( root, "pilots" ), false );
        if( update )
            WriteText( stamp, version );
    }

    private void CopyAssets( AssetManager assets, String path, File dest, boolean overwrite )
    {
        try
        {
            String[] children = assets.list( path );
            if( children != null && children.length > 0 )
            {
                dest.mkdirs();
                for( String child : children )
                    CopyAssets( assets, path + "/" + child, new File( dest, child ), overwrite );
                return;
            }
            if( dest.exists() && ! overwrite )
                return;
            try( InputStream in = assets.open( path ); OutputStream out = new FileOutputStream( dest ) )
            {
                byte[] buf = new byte[ 16384 ];
                int n;
                while( (n = in.read( buf )) > 0 )
                    out.write( buf, 0, n );
            }
        }
        catch( Exception e )
        {
            android.util.Log.e( "Forsaken", "installing " + path + ": " + e );
        }
    }

    private static String ReadText( File f )
    {
        try( InputStream in = new java.io.FileInputStream( f ) )
        {
            byte[] buf = new byte[ 64 ];
            int n = in.read( buf );
            return n > 0 ? new String( buf, 0, n ).trim() : "";
        }
        catch( Exception e )
        {
            return "";
        }
    }

    private static void WriteText( File f, String text )
    {
        try( OutputStream out = new FileOutputStream( f ) )
        {
            out.write( text.getBytes() );
        }
        catch( Exception e )
        {
        }
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
