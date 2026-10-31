-keep class com.grangedevgroup.mapkern.Http { *; }
-keep class com.grangedevgroup.mapkern.Http$Response { *; }
-keepclassmembers class com.grangedevgroup.mapkern.MapView {
    native <methods>;
    void requestRender();
    void onNativeEvent(java.lang.String, java.lang.String);
}
