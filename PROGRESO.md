# CamSolid: progreso

Última actualización: 2026-11-11 (fin de la sesión)

## Objetivo
CamSolid convierte el celular en una webcam para el PC, con el menor tiempo de respuesta posible. Las dos partes son apps nativas e instalables, no una app web.

Más adelante se podría agregar audio y comercializarla.

## Estado actual
**Funciona de punta a punta por USB** con un Redmi Note 14 (Android 16, chip MediaTek): Windows muestra la cámara "CamSolid (Windows Virtual Camera)" y entrega el video del celular a **1920x1080 y 30 cuadros por segundo**. Este es el formato por defecto, elegido por el usuario; 1280x720 queda como alternativa.

`CamSolid.exe` ya es una app de escritorio con ventana: muestra la cámara de Windows, el celular conectado, qué aplicación usa la cámara y la latencia. También tiene un botón para instalar la app en el celular.

**Ya hay instalador:** `dist\CamSolid-0.1.0.zip` (5,3 MB). Instala en `C:\Program Files\CamSolid`, registra la cámara y crea los accesos directos y la entrada de desinstalación. Se probó la instalación; la desinstalación aún no.

### Arranque (tiempo desde que una aplicación abre la cámara hasta el primer cuadro del celular)
| Caso | Antes | Ahora |
|---|---|---|
| Cámara del celular apagada | 1,4 s | 1,4 s (lo que tarda el celular en encenderla) |
| Cámara del celular encendida (dentro de los 60 s tras la última desconexión) | 0,77 s | **0,11 s** |

La app del celular mantiene la cámara encendida 60 s después de que el PC se desconecta. Al reconectar, envía los parámetros del códec, pide un cuadro clave y descarta los cuadros previos, así que el PC nunca recibe imagen corrupta.

### Latencia medida (2026-11-11, habitación con poca luz)
| Tramo | 1920x1080 | 1280x720 |
|---|---|---|
| Cámara del celular (del sensor al cuadro listo) | 73 ms, de ellos 30 ms de exposición | — |
| Codificador del celular (MediaTek) | 21 ms | — |
| PC: transporte, decodificación y entrega | 7-9 ms (decodificación 5 ms) | 5 ms |
| **Total** | **unos 100-105 ms** | **unos 81 ms** |

Con el simulador de celular, que usa el codificador del propio PC, el total es de 2,5 ms: el PC no es el cuello de botella.

### Pruebas para bajar la latencia
| Prueba | Resultado |
|---|---|
| 60 cuadros por segundo | El Redmi no lo admite: su máximo es 30 en todos los rangos |
| Plantilla de vista previa en vez de grabación | Sin cambio |
| Estabilización desactivada y reducción de ruido rápida | Sin cambio |
| Sin vista previa en la pantalla del celular | Sin cambio |
| Ajustes de MediaTek: `wfd.low-latency-enabled`, `wfd.feature-on`, `vpp.disabled` | Sin cambio |

La latencia de la cámara es procesado interno del celular, más la exposición. **Con más luz baja sola**, porque la cámara expone menos tiempo; con poca luz expone 30 ms por cuadro. Las opciones probadas quedan disponibles para probar otros celulares (ver "Cómo compilar y probar").

La calidad en 1080p es buena: en un recorte a tamaño real se ven texturas y detalles finos sin bloques de compresión, a unos 15,5 Mbit/s.

## Decisiones
| Tema | Decisión | Motivo |
|---|---|---|
| Celular | Android nativo en Kotlin (Camera2 y MediaCodec), sin bibliotecas externas | Acceso directo a la cámara y al codificador por hardware; APK de 873 KB |
| PC | Windows 11 en C++ (Media Foundation) | Crea la cámara virtual sin driver de kernel |
| Video | H.264 por hardware, perfil Baseline, sin cuadros B | Ningún cuadro espera a otro |
| Conexión | USB con `adb forward`, puerto 27183; Wi-Fi después | Latencia baja y estable |
| Decodificación | Dentro de la DLL, en el servicio Frame Server | Evita pasar el video por otro proceso |
| Cuadros atrasados | Se descartan; siempre se entrega el último | Nunca se acumula retraso |
| Audio | Para una fase posterior | Un micrófono virtual requiere un driver de audio firmado |
| iPhone | Después de Android | Hace falta un Mac con Xcode y una cuenta de pago de Apple Developer |

Limitación conocida: esta cámara virtual solo funciona en Windows 11.

## Cómo funciona
```
Celular (app CamSolid)                      PC
Camera2 ──> codificador H.264 ──> TCP 27183 ═══ USB (adb forward) ═══> CamSolidSource.dll
                                                                       (dentro de Frame Server)
                                                                       decodifica ──> cámara "CamSolid"
                                                                                         │
                                                                     Zoom, Teams, OBS <──┘
```

- `CamSolid.exe` es la app de escritorio. Crea la cámara virtual, mantiene el `adb forward` hacia el celular en un hilo propio y muestra el estado en su ventana.
- La DLL publica su estado (formato en uso, celular, latencia, cuadros por segundo) en la memoria compartida `Global\CamSolidStatus`, que `CamSolid.exe` lee cada medio segundo. Es global porque la DLL corre en el servicio Frame Server, en otra sesión de Windows.
- Cuando una aplicación abre la cámara, la DLL se conecta al celular y le pide el tamaño y los cuadros por segundo que esa aplicación eligió.
- Si no hay celular, la cámara muestra barras de colores.
- El PC mide la latencia sincronizando relojes con mensajes Ping y Pong, y se la envía al celular cada segundo para que la muestre.

## Protocolo
Definido en `pc/common/Protocol.h` y `android/.../Protocol.kt`. Cada mensaje es una cabecera de 16 bytes en big-endian (`type`, `size`, `time`) seguida de los datos.

| Tipo | Dirección | Contenido |
|---|---|---|
| 1 StreamRequest | PC → celular | versión, ancho, alto, cuadros por segundo, bitrate |
| 2 Ping | PC → celular | `time` = reloj del PC |
| 3 Stats | PC → celular | latencia, cuadros por segundo, decodificación, espera |
| 101 CodecConfig | celular → PC | SPS y PPS (Annex B) |
| 102 Frame / 103 KeyFrame | celular → PC | cuadro H.264; `time` = captura en el reloj del celular |
| 104 Pong | celular → PC | `time` del Ping y reloj del celular |
| 105 DeviceInfo | celular → PC | nombre del dispositivo |

## Estructura
```
D:\CamSolid
├── PROGRESO.md
├── .gitignore
├── pc                              App de Windows (C++, CMake)
│   ├── CMakeLists.txt
│   ├── common
│   │   ├── CamSolidIds.h           Identificador y nombre de la cámara
│   │   ├── Protocol.h              Protocolo con el celular
│   │   ├── Status.h                Estado compartido entre la DLL y la app
│   │   └── Clock.h                 Reloj en microsegundos
│   ├── source                      CamSolidSource.dll, la cámara virtual
│   │   ├── DllMain.cpp, CamSolidSource.def   Entrada y registro COM
│   │   ├── Activator.*             Crea la cámara cuando Windows la pide
│   │   ├── MediaSource.*           La cámara ante Windows
│   │   ├── MediaStream.*           Entrega los cuadros
│   │   ├── PhoneReceiver.*         Conexión con el celular y estadísticas
│   │   ├── StatusPublisher.*       Publica el estado para la app
│   │   ├── H264Decoder.*           Decodificador de baja latencia
│   │   ├── Nv12.*                  Copia y escala de cuadros
│   │   ├── TestPattern.*           Barras de colores sin celular
│   │   └── Attributes.h            Base común de los objetos COM
│   ├── app                         CamSolid.exe, la app de escritorio
│   │   ├── main.cpp                Ventana, cámara virtual y estado
│   │   └── Adb.*                   Busca el celular y mantiene adb forward
│   ├── tools
│   │   ├── probe.cpp               CamSolidTest.exe: lee la cámara como cualquier aplicación
│   │   ├── selftest.cpp            Carga la DLL sin registrarla (desarrollo)
│   │   ├── fakephone.cpp           Simulador de celular (desarrollo)
│   │   ├── Bitmap.cpp              Guarda cuadros como BMP
│   │   ├── medir-latencia.ps1      Mide la latencia con el celular, por tramos
│   │   └── captura-ventana.ps1     Guarda una imagen de la ventana de CamSolid
│   ├── build                       Generado al compilar
│   └── bin                         Copia de desarrollo (ya no está registrada; ver instalador)
├── instalador
│   ├── empaquetar.ps1              Compila todo y arma dist\CamSolid y el ZIP
│   ├── Instalar.cmd                Doble clic: abre instalar.ps1 con permisos de administrador
│   ├── instalar.ps1                Instala en Program Files, registra la cámara, accesos directos
│   ├── desinstalar.ps1             Lo deshace todo
│   └── LEEME.txt                   Instrucciones para el usuario final
├── dist                            Generado por empaquetar.ps1 (ignorado por git)
└── android                         App del celular (Kotlin, Gradle 9.9.1, plugin de Android 9.5.1)
    └── app/src/main/java/com/camsolid/app
        ├── MainActivity.kt         Pantalla: vista previa, estado y latencia
        ├── CameraStreamer.kt       Camera2 + codificador H.264
        ├── StreamServer.kt         Escucha al PC y contesta los Ping
        └── Protocol.kt
```

## Cómo compilar y probar
PC, con Visual Studio Community 2026:
```
cmake -S D:\CamSolid\pc -B D:\CamSolid\pc\build -G "Visual Studio 18 2026" -A x64
cmake --build D:\CamSolid\pc\build --config Release
```

Android, con el JDK de Android Studio:
```
set JAVA_HOME=D:\AndroidStudio\jbr
cd D:\CamSolid\android
gradlew assembleDebug
adb install -r app\build\outputs\apk\debug\app-debug.apk
```
En Xiaomi hay que activar **Instalar vía USB** en las opciones de desarrollador, y no se puede usar `adb install -g`.

Uso:
1. Abrir la app CamSolid en el celular, conectado por USB con la depuración activada.
2. Abrir `D:\CamSolid\pc\bin\CamSolid.exe` en el PC. Si se abre otra vez, muestra la ventana que ya estaba abierta; `CamSolid.exe --stop` la cierra.
3. Elegir la cámara "CamSolid" en cualquier aplicación.

Pruebas:
- `CamSolidTest.exe imagen.bmp`: lee 3 s de la cámara a través de Windows y guarda un cuadro a los 2,5 s.
- `CamSolidSelfTest.exe build\Release\CamSolidSource.dll imagen.bmp 8 0`: prueba la DLL recién compilada sin registrarla (último número: 0 = 1080p, 1 = 720p).
- `CamSolidFakePhone.exe 20` con `CamSolid.exe --sin-adb`: prueba el PC sin celular.
- `tools\medir-latencia.ps1 -Opciones "--ez estabilizacion true"`: mide la latencia por tramos con el celular. Opciones de la app: `plantillaVistaPrevia`, `estabilizacion`, `ruidoRapido`, `vistaPrevia`, `mtkBajaLatencia`, `mtkWifiDisplay`, `mtkSinPreprocesado`.
- `tools\captura-ventana.ps1 ventana.png`: imagen de la ventana de CamSolid.

### Empaquetar e instalar
```
powershell -ExecutionPolicy Bypass -File D:\CamSolid\instalador\empaquetar.ps1 -Version 0.1.0
```
Genera `D:\CamSolid\dist\CamSolid\` y `dist\CamSolid-0.1.0.zip`. Para instalar, doble clic en `dist\CamSolid\Instalar.cmd`; pide permisos de administrador.

El instalador hace lo siguiente:
1. Cierra CamSolid si está abierto.
2. Detiene Frame Server, porque mantiene la DLL cargada.
3. Copia todo a `C:\Program Files\CamSolid`.
4. Registra la DLL con `regsvr32`.
5. Vuelve a arrancar Frame Server.
6. Crea los accesos directos del menú Inicio y del escritorio público, y la entrada en "Agregar o quitar programas".

Deja un registro en `%TEMP%\CamSolid-instalar.log`. La desinstalación (`desinstalar.ps1`, también desde Configuración > Aplicaciones) deja el suyo en `%TEMP%\CamSolid-desinstalar.log`. Los dos aceptan `-SinPausa` para las pruebas automáticas.

**Para probar cambios de la DLL** basta con empaquetar y volver a instalar. La cámara registrada es la de Program Files; `pc\bin` ya no se usa. Para iterar sin reinstalar, sirve `CamSolidSelfTest`, que carga la DLL sin registrarla.

La APK del paquete es la de depuración, firmada con la clave de desarrollo. Para distribuir CamSolid hará falta una firma propia.

## Control de versiones
- **Repositorio privado:** https://github.com/Drayer-dm/CamSolid. Solo el dueño de la cuenta puede verlo.
- **Rama principal:** `main`. Para un cambio grande, conviene trabajar en una rama (`feature/<tema>`) y fusionarla en `main` cuando esté probado.
- **Qué no se sube** (ver `.gitignore`): compilados (`pc/build`, `pc/out`, `pc/bin`, `android/**/build`), paquetes (`dist/`), APK, configuración local (`android/local.properties`, `.vs`, `.idea`) y cachés de Gradle.

### Numeración de versiones
Se usa `MAYOR.MENOR.PARCHE`:
- **PARCHE** sube con correcciones que no cambian el uso (0.1.0 → 0.1.1).
- **MENOR** sube con funciones nuevas (0.1.x → 0.2.0).
- **MAYOR** pasa a 1 con la primera versión para el público, y vuelve a subir si se rompe la compatibilidad entre el celular y el PC.

La versión se fija en dos lugares que deben coincidir:
1. `instalador\empaquetar.ps1 -Version X.Y.Z`, que la escribe en `version.txt` del paquete y en "Agregar o quitar programas".
2. `android\app\build.gradle.kts`: `versionName = "X.Y"` y `versionCode`, que debe subir en cada APK publicada.

Si cambia el protocolo entre el celular y el PC, también se sube `CAMSOLID_PROTOCOL_VERSION` en `pc/common/Protocol.h` y `Protocol.VERSION` en `Protocol.kt`.

### Cómo trabajar en cada sesión
1. Al empezar: `git pull`.
2. Hacer commits pequeños, con un mensaje en español que diga qué cambió y por qué.
3. Al terminar: actualizar `PROGRESO.md` (estado, pendientes y registro) y hacer `git push`.
4. Al publicar un paquete: empaquetar, hacer commit y crear una etiqueta: `git tag -a v0.2.0 -m "..."` y `git push --tags`. Anotar la versión en la tabla de abajo.

### Versiones publicadas
| Versión | Fecha | Contenido |
|---|---|---|
| 0.1.0 | 2026-11-11 | Primera versión funcional: cámara virtual, app de Android, app de escritorio, 1080p a 30 por USB, arranque en caliente e instalador |

## Pendientes
Para la próxima sesión:
- **Probar la desinstalación** y reinstalar después. Solo se probó la instalación.
- **Comprobar el resultado del botón "Instalar app en el celular".** Se vio que empieza a instalar ("Instalando la app en el celular…"), pero no se llegó a ver el mensaje final.
- **Dos copias de `adb`:** con Android Studio instalado, el `adb` del SDK ya estaba corriendo y la app usó ese servidor. Hay que comprobar qué pasa si las versiones difieren: `adb` reinicia el servidor y eso podría cortar la conexión un momento.

Después:
- **Arranque en frío** (1,4 s): se podría encender la cámara del celular en cuanto `CamSolid.exe` lo detecte, a costa de batería.
- **Cámara que sobreviva al cerrar `CamSolid.exe`** (`MFVirtualCameraLifetime_System`), e icono en la bandeja del sistema.
- **Latencia:** en este celular ya no hay ajustes que la bajen. Falta probar otros modelos, sobre todo con chip Qualcomm, y medir con buena luz.
- **Distribución:** firma propia de la APK, instalador MSI y revisar la licencia para redistribuir `adb` (incluido con su `NOTICE`).
- Wi-Fi.
- Interfaz de la app del celular y del PC; por decisión del usuario, después de que la cámara esté terminada.
- Comprobar la cámara frontal y la imagen al girar el celular al otro lado horizontal.

## Registro
### 2026-11-06
- Se definieron el objetivo, las decisiones técnicas y las fases.
- Se instalaron Visual Studio Community 2026 y Android Studio.
- El usuario escribió a mano `CMakeLists.txt`, `CamSolidIds.h` y `CamSolidSource.def`.
- La fase 1 se armó y se probó en una carpeta temporal.

### 2026-11-11
- A pedido del usuario, Claude pasó a desarrollar y probar directamente.
- Fase 1: la DLL se registró y Windows enumeró la cámara, que entregó la imagen de prueba a 30 cuadros por segundo a través de Frame Server.
- Fase 3 (PC): se añadieron el protocolo, el receptor, el decodificador H.264 de baja latencia, la entrega inmediata y el simulador de celular. Con el simulador, la latencia es de 2,5 ms.
- Fase 2 (Android): la app se creó y se probó primero en el emulador y luego en el Redmi Note 14.
- Errores corregidos: un desbordamiento en la sincronización de relojes; la parada lenta del receptor; un codificador que rechazaba `KEY_PREPEND_HEADER_TO_SYNC_FRAMES`; Xiaomi, que usa otro reloj en las marcas de tiempo; y la vista previa girada.
- `CamSolid.exe` gestiona `adb forward` de forma automática.
- Prueba final: Windows entrega el video real del celular a través de Frame Server, a 30 cuadros por segundo y con 86 ms de latencia.
- El usuario fijó 1920x1080 a 30 como formato por defecto; los formatos de 60 se quitaron.
- La latencia se midió por tramos (cámara, codificador y PC) y se probaron 60 cuadros por segundo, la plantilla de captura, la estabilización, la reducción de ruido, la vista previa y tres ajustes de MediaTek. Ninguno la bajó, y en 1080p queda en unos 100-105 ms.
- `CamSolid.exe` pasó a ser una app de escritorio con ventana, que lee el estado que publica la DLL. Se probó en tres estados (sin uso, transmitiendo y después) tanto con la DLL cargada directamente como a través de Frame Server.
- Arranque: la DLL anota el tiempo hasta el primer cuadro (`startupMicros` en `Status.h`). La app del celular mantiene la cámara encendida 60 s tras desconectarse y, al reconectar, pide un cuadro clave. Con la cámara ya encendida, el arranque bajó de 0,77 s a 0,11 s; en frío sigue en 1,4 s.
- Se añadió el botón "Instalar app en el celular" (`AdbForwarder::InstallApp`). Detecta el bloqueo de Xiaomi y explica cómo resolverlo.
- Se creó el instalador (`instalador\`). El paquete 0.1.0 se instaló en este PC: la cámara quedó registrada desde Program Files y funcionó en 1080p a 30 cuadros por segundo a través de Windows.
- Queda abierto `C:\Program Files\CamSolid\CamSolid.exe`, la versión instalada.
- Se creó el repositorio privado en GitHub (`Drayer-dm/CamSolid`, rama `main`) con la etiqueta `v0.1.0`, y se documentó cómo llevar el control de versiones.
