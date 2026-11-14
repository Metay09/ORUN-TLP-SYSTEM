package com.getcapacitor;
public class PluginCall {
    public void resolve(JSObject data) { }
    public void resolve() { }
    public void reject(String msg, String code, Exception ex, JSObject data) { }
    public void reject(String msg, Exception ex, JSObject data) { }
    public void reject(String msg, String code, JSObject data) { }
    public void reject(String msg, String code, Exception ex) { }
    public void reject(String msg, JSObject data) { }
    public void reject(String msg, Exception ex) { }
    public void reject(String msg, String code) { }
    public void reject(String msg) { }
    public void unimplemented() { }
    public void unavailable() { }
    public String getMethodName() { return null; }
    public JSObject getData() { return null; }
    public String getString(String name) { return null; }
    public String getString(String name, String defaultValue) { return null; }
    public Integer getInt(String name) { return null; }
    public Integer getInt(String name, Integer defaultValue) { return null; }
    public Long getLong(String name) { return null; }
    public Long getLong(String name, Long defaultValue) { return null; }
    public Double getDouble(String name) { return null; }
    public Boolean getBoolean(String name) { return null; }
    public Boolean getBoolean(String name, Boolean defaultValue) { return null; }
    public JSObject getObject(String name) { return null; }
    public JSArray getArray(String name) { return null; }
    public JSArray getArray(String name, JSArray defaultValue) { return null; }
    public boolean hasOption(String name) { return false; }
    public void setKeepAlive(Boolean keepAlive) { }
}
