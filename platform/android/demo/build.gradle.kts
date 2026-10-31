plugins {
    id("com.android.application")
}

android {
    namespace = "com.grangedevgroup.mapkern.demo"
    compileSdk = 37

    defaultConfig {
        applicationId = "com.grangedevgroup.mapkern.nativedemo"
        minSdk = 24
        targetSdk = 36
        versionCode = 1
        versionName = "0.1.0"
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"))
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

dependencies {
    implementation(project(":mapkern"))
}
