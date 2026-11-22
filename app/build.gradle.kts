plugins {
    id("com.android.application")
}

android {
    namespace = "com.oll.ue4esp"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.oll.ue4esp"
        minSdk = 24
        targetSdk = 33
        versionCode = 1
        versionName = "1.0.0"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
