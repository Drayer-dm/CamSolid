plugins {
    id("com.android.application")
}

android {
    namespace = "com.camsolid.app"
    compileSdk = 37

    defaultConfig {
        applicationId = "com.camsolid.app"
        // Android 8.0: primera versión con los ajustes de baja latencia del codificador
        minSdk = 26
        targetSdk = 37
        versionCode = 1
        versionName = "0.1.0"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }
}
