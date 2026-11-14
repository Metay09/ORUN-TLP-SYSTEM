package com.getcapacitor;
import android.content.Context;
public class Plugin {
    protected Bridge bridge;
    public Plugin() { }
    public void load() { }
    public Context getContext() { throw new RuntimeException("stub"); }
    public android.app.Activity getActivity() { throw new RuntimeException("stub"); }
    public Bridge getBridge() { throw new RuntimeException("stub"); }
    protected void notifyListeners(String eventName, JSObject data, boolean retainUntilConsumed) { }
    protected void notifyListeners(String eventName, JSObject data) { }
    protected boolean hasListeners(String eventName) { return false; }
    protected void handleOnDestroy() { }
    protected void handleOnResume() { }
    protected void handleOnPause() { }
    public void execute(Runnable runnable) { }
}
