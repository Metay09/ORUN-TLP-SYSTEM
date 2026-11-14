package com.getcapacitor;
import org.json.JSONException;
import org.json.JSONObject;
public class JSObject extends JSONObject {
    public JSObject() { }
    public JSObject(String json) throws JSONException { super(json); }
    public static JSObject fromJSONObject(JSONObject obj) throws JSONException { return null; }
    @Override public String getString(String key) { return null; }
    public String getString(String key, String defaultValue) { return null; }
    public Integer getInteger(String key) { return null; }
    public Integer getInteger(String key, Integer defaultValue) { return null; }
    public Boolean getBoolean(String key, Boolean defaultValue) { return null; }
    public Boolean getBool(String key) { return null; }
    public JSObject getJSObject(String name) { return null; }
    @Override public JSObject put(String key, boolean value) { return this; }
    @Override public JSObject put(String key, int value) { return this; }
    @Override public JSObject put(String key, long value) { return this; }
    @Override public JSObject put(String key, double value) { return this; }
    @Override public JSObject put(String key, Object value) { return this; }
    public JSObject put(String key, String value) { return this; }
}
