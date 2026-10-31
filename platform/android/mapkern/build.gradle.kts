plugins {
    id("com.android.library")
}

android {
    namespace = "com.grangedevgroup.mapkern"
    compileSdk = 37
    ndkVersion = "30.0.16248370"

    defaultConfig {
        minSdk = 24
        externalNativeBuild {
            cmake {
                arguments += listOf("-DMAPKERN_TESTS=OFF", "-DMAPKERN_VIEWER=OFF", "-DMAPKERN_SHARED=OFF", "-DANDROID_STL=c++_shared")
                targets += "mapkern-jni"
            }
        }
        ndk {
            abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64")
        }
        consumerProguardFiles("consumer-rules.pro")
    }

    externalNativeBuild {
        cmake {
            path = file("../../../CMakeLists.txt")
            version = "4.4.4"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
