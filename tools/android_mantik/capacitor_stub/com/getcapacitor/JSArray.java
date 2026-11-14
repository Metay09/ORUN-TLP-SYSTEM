package com.getcapacitor;
import java.util.Collection;
import java.util.List;
import org.json.JSONArray;
import org.json.JSONException;
public class JSArray extends JSONArray {
    public JSArray() { }
    public JSArray(String json) throws JSONException { super(json); }
    public JSArray(Collection copyFrom) { super(copyFrom); }
    public <E> List<E> toList() throws JSONException { return null; }
}
