# HEROLTE (Galaxy S7 SM-G930F, Exynos 8890): progreso

Registrar por separado: compilación, marcadores recuperados, lo que se ve en
pantalla y comportamiento de iOS. Plan y mapa de hardware en
`docs/herolte-bringup.md`.

## 2026-10-20

- Dispositivo: SM-G930F, bootloader desbloqueado, TWRP instalado.
- `shim/herolte/probe.S` + `probe.c` compilados con Arm GNU Toolchain 14.2.rel1
  (`aarch64-none-elf`): Image de 10.784 bytes, solo relocaciones PC-relativas
  (ADR_PREL_PG_HI21, ADD_ABS_LO12_NC, CALL26), sin llamadas a librería.
  **Solo compilación**; no se ejecutó en el teléfono.
- `tools/herolte-pack-boot.py` probado con una imagen BOOT sintética
  (página de 2048, DTBH v2 con chip 8890): conserva DTBH y ramdisk, copia
  `text_offset`/flags del kernel de stock, recalcula el SHA-1 del header.
  Todavía no se probó con el BOOT real del S7.
- BOOT del teléfono (custom ROM, no stock), leído con `dd` desde TWRP:
  partición de 41.943.040 bytes, página 2048, kernel arm64 sin comprimir de
  38 MB con `text_offset=0x80000`, DTBH v2 con 3 DTB Exynos 8890
  (hw_rev 4–7, 8, 9). DTB: RAM `0x80000000+0x7e400000` y `0x880000000+2G`,
  initrd en `0x82000000`, reservas TIMA `0xb1000000`, ION desde
  `0xc0000000`, módem `0xf0000000`. `0xe2a00000` cae en un hueco libre.
- `probe-boot.img` armado desde ese BOOT (1.495.056 bytes).
- **Primera prueba en el teléfono (reporte del usuario):** el probe muestra
  texto blanco sobre el logo y la cuenta "TWRP EN 60 S"; al terminar, el
  teléfono reinicia solo a TWRP. Prueba que S-Boot ejecuta nuestro Image
  (header arm64 + DTBH correctos), que el framebuffer `0xe2a00000` con el HW
  trigger de DECON_F funciona, que CNTPCT avanza y que el reinicio a
  recovery por PMU `0x105c0000` (INFORM2/INFORM3/SWRESET) funciona.
- **Foto del probe:** EL 1; SCTLR `0x30c5183e` (MMU off); FDT `0x8a000000`;
  IMAGE `0x800bc000`; MIDR `0x410fd034` (Cortex-A53 r0p4); MPIDR
  `0x80000100`; MMFR0 `0x1122` (sin 16 KiB); PFR0 `0x2222`; ISAR0 `0x11120`
  (AES/SHA1/SHA2/CRC32, **sin LSE**); CNTFRQ **0** (como el A5: frecuencia
  desde el ADT, 26 MHz); DECON trigger previo `0x12b0`; initrd
  `0x89000000..0x890dbf7e` (el ramdisk de la ROM).
- Consecuencia: entre initrd y FDT hay 16 MiB, poco para el payload de XNU.
  `xnu_boot` ahora busca el payload pegado a su Image
  (`herolte-pack-boot.py --append`) y usa el initrd solo como alternativa.
- Serie de parches normalizada sobre `xnu-8796.141.3` (ver
  `docs/herolte-bringup.md`): 123 parches aplican con `git apply` simple,
  incluido `0200-herolte-board-target`. 12 movidos a `xnu/patches-pending/`
  (los 10 rotos m?s `0133`/`0134` de GPU del A5, publicados despu?s).
  **El kernel HEROLTE todavía no se compiló**: Actions pendiente.
- `shim/herolte/xnu_boot` (loader) y `tools/mkadt_herolte.py` escritos y
  compilados/ejecutados localmente; sin prueba en el teléfono.
- `0106`/`0107` reescritos como `0201-herolte-amfi-bridge` y
  `0202-herolte-4k-unsigned-dyld-cache`; 125 parches aplican en orden.
- **Primer kernel HEROLTE compilado en Actions** (fork `wardevsa/ios-a5xelte`,
  rama `herolte`, run 39477885586, commit fb6194b): artefacto `xnu-kernel`
  de 61 MB. El chequeo de LSE encontró una sola coincidencia, dentro de
  `K256` (constantes de SHA-256 en `__TEXT`), es decir, datos: el código del
  kernel no tiene atómicas LSE. El chequeo ahora ignora `K256`.
- Actions publica los errores de compilación como anotaciones públicas
  (los logs requieren sesión de GitHub).
- **Primer arranque de XNU en el S7** (`xnu-boot.img`: loader + payload
  pegado de 16.474.720 bytes; kernel sha256 `61a9dd49…ca5d`, cmdline
  `debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress`).
  Foto del usuario: consola de XNU en el framebuffer con VM bootstrap,
  zone_init, firehose, scheduler, KDP serial, `iBoot version: herolte-entry`,
  `SWEET PRNG`, `A5XELTE pthread: libpthread-514.100.2 registered`,
  `SWEET AMFI: built-in interface registered`, platform expert iniciado.
  Pánico en `SweetCPU.cpp:86`: `unsupported topology cpu=0 mpidr=0x100
  count=1` (el chequeo esperaba MPIDR 0 del A5).
- `0203-herolte-boot-mpidr`: el MPIDR de arranque es constante de placa
  (`A5XELTE_BOOT_MPIDR`: A5 `0x0`, herolte `0x100`).
- **Kernel completo hasta la espera del root** (run 39481021599, kernel
  sha256 `ffa3d8ea…432f`, misma cmdline). Foto del usuario: `A5XELTE CPU:
  cpu=0 mpidr=0x100 came alive; SGI1 OK`, `1/1 CPUs initialized, SGI1
  verified`, BSD init, AppleKeyStore, IOWatchdog, IOCoreSurfaceRoot, HID de
  botones/touch, SWEET AMFI (0201) y Sandbox registrados, HFS 627.100.6,
  self-tests de Skywalk `PASSED`, `lo0`/`gif0`/`stf0`, y finalmente
  `Waiting on <dict>IOProviderClass IOMedia, Content Apple_HFS</dict>`.
  Es el punto esperado: no hay driver de almacenamiento todavía.
- **Primer probe UFS** (`0204`, run 39484347438, kernel `d3905386…45f2`,
  `ufsprobe=1`): `cap 0x383ff0f ver 0x200 hcs 0x10f hce 0x1 is 0
  utrlba 0x0:ef002000 dbr 0 rsr 1`, `txprdt 0xc rxprdt 0xc nexus 0xffff`.
  El host quedó vivo tras S-Boot y tomó nuestra lista. TEST UNIT READY en
  LUN 0 (3 veces): `ocs 0x7 upiu 0x21 response 0x1 status 0x2`, o sea un
  UPIU de respuesta real con CHECK CONDITION: el transporte (UTRD con
  offsets en bytes, PRDT, NEXUS, doorbell) funciona. El probe no siguió a
  READ porque abortaba tras el TUR. Siguiente: imprimir el sense, probar
  LUN 0..7 y leer LBA1 aunque falle el TUR.
- **Segundo probe UFS** (run 39486812960, kernel `00f0b0da…deb1`): LUN 0–3
  responden NOT READY (sense key 2, ASC/ASCQ 0/0); LUN 4–7 ILLEGAL REQUEST
  ASC 0x25 (no existen: el S7 tiene 4 LU). READ(10) LBA1 en LUN 0: NOT
  READY. Hipótesis: S-Boot deja el dispositivo en un modo de bajo consumo;
  Linux lo reactiva con START STOP UNIT (ACTIVE) al W-LUN UFS Device
  (`0xD0`). `0204` ahora lo hace antes del primer comando (probe e init).
- **Tercer probe UFS: lectura funcionando** (run 39489906319, kernel
  `869c4c5f…d74a`). W-LUN `0xD0` TUR: NOT READY ASC/ASCQ 04/02
  ("initializing command required"); `START STOP UNIT (ACTIVE) 0`; LUN 0–3
  TEST UNIT READY 0; `READ(10) LBA1 0, signature EFI PART`; GPT de 128
  entradas de 128 bytes en LBA 2; particiones p1 `BOTA0` … p17 `USERDATA`
  (~21,5 GB, bloques de 4 KiB). Primera lectura del UFS desde XNU.
- **Primer root montado desde UFS.** Volumen HFSX de 32 MiB
  (`hfs-test-image`, sha256 `043f1459…8b94`) escrito por adb/TWRP al inicio
  de USERDATA (`/dev/block/sda18`); BOOT con `rd=ufsroot0 ufsroot=USERDATA
  cs_enforcement_disable=1` (mismo kernel `869c4c5f…`); ambos verificados por
  readback. Foto: `"USERDATA" on LUN 0: first block 0x22e0ac, 21566 MiB,
  read-only`, `BSD root: ufsroot0, major 1, minor 0`, `hfs: mounted HEROTEST
  on device b(1, 0)`, `root volume is not authenticated (error 25);
  continuing unsealed`, `devfs_kernel_mount: kernel_mount failed: 2` (la
  imagen no tiene `/dev`), y pánico esperado `exec of /sbin/launchd failed,
  errno 2`. Siguiente: imagen raíz de iOS 16.
- **Root de iOS 16.7.16 (iPhone X, D22AP) en USERDATA.** Workflow `ios-root`
  en el repo privado `wardevsa/ios-herolte-root` (la imagen es software de
  Apple; no se publica): HFSX de 20480 MiB, 3 partes gzip verificadas por
  SHA-256. Escrita con `tools/herolte-flash-root.py` (bloques de 64 MiB con
  readback SHA-256, ceros escritos en el teléfono; un corte del servidor adb
  se retomó con el archivo de estado): 320/320 bloques, imagen sha256
  `2e200c51…13ca8`, firma `HX` en el offset 1024. ADT con `/product` real de
  D22AP (63 propiedades) y kernel `d706ccd8…e271` (incluye 0205). Boot:
  `rd=ufsroot0 ufsroot=USERDATA ufsrw=1 rootrw=1 cs_enforcement_disable=1
  unentitled_ios_sim_launch=1 ignition_level=1 launchd_unsecure_cache=1
  wdt=-1 -wdt_no_pmu wdt_test=no_user_checkin`, verificado por readback.
- **Primer arranque de iOS** (log persistente 0206, kernel `012d056a…af10`,
  53 flushes): launchd hace todo el early boot (fsck, mount-phase-1/2,
  keybag, obliteration, launchd_cache_loader con `launchd_unsecure_cache`) y
  llega a `Early boot complete`; lanza SpringBoard, backboardd, keybagd,
  bluetoothd, fairplayd.H2, etc. SpringBoard aborta en loop (24 veces):
  `NSInternalInconsistencyException: We expect a valid bounds for
  configuration ... Main-External ... CADisplay.name = Wireless ... bounds
  {{0,0},{0,0}}` — no había display principal: el `disp0` del ADT decía
  `samsung,herolte-display` y AppleCLCD matchea `samsung,a5xelte-display`.
- **Con el ADT corregido** (boot `67953d9b…ea19`, mismo kernel): `SWEET FB:
  AppleCLCD 1440x2560 stride 5760 at 0xe2a00000 ... presented as 1125x2436`,
  backboardd configura el display (`display on`) y crea superficies
  1440x2560 y 1125x2436; **SpringBoard ya no crashea** (solo CommCenter, sin
  módem, y driverkitd). En ~5 min de log todavía no hay ningún `swap`.
- **Arranque largo (~20 min) con el ADT corregido:** log hasta el flush #37
  (46 KB). backboardd hace `SwapBegin` (sel 4) y sel 52, pero nunca
  `SwapEnd` (sel 5): ningún `SWEET FB: swap`, ninguna registración de
  notificaciones de vsync. El log deja de crecer bastante antes del final.
  `0207`: sampler de hilos (`sweet_sample.c`, SpringBoard/backboardd, a los
  90 s y cada 120 s) y latido `HEROLTE LOG: alive N s` cada 60 s.
- **Muestreos 0207** (kernel con sampler, ~14 min, latido estable cada
  60 s: el sistema no está colgado). Simbolizado contra la dyld cache de la
  imagen: el hilo `IOMFBServer` de backboardd rasteriza con el renderer de
  CPU de QuartzCore (`CA::OGL::SW::scanline`, `vAlpha_Blend` de vImage) y
  reintenta `eagl_init` (busca GPU) en cada `render_for_time`; el hilo
  principal de backboardd queda en `CA::Display::Display::update` (mach_msg)
  y SpringBoard espera en `FBSystemShellInitialize` (check-in síncrono con
  backboardd). Con un solo A53 el primer frame no termina.
- `0208`: SMP en los 4 A53 (MPIDR `0x100..0x103`) por PSCI `CPU_ON`, IPIs
  GICv2 dirigidas por la máscara de ITARGETSR0 de cada núcleo (adaptado de
  los pendientes 0112/0113 del A5). Requiere `cpus=4`.
- **Primer arranque con 4 núcleos** (`cpus=4`, boot `77af8df8…d298`):
  foto `A5XELTE CPU: cpu=1..3 PSCI CPU_ON rc=0 ... came alive; SGI1 OK`,
  `4/4 CPUs initialized, SGI1 verified`; el arranque va notablemente más
  rápido. Log (flush #67, 11 min): el hilo principal de backboardd ya no se
  bloquea; ahora espera en `DMPerformMigration` (XPC síncrono) mientras
  `com.apple.migrationpluginwrapper` aborta (3 veces), y SpringBoard sigue en
  `FBSystemShellInitialize`. CommCenter crashea 45 veces (sin módem). Sin
  `swap` todavía.
- `tools/herolte-tune-ios-root.sh` (en el workflow `ios-root`): como las
  imágenes v12/v13 del donor, `LastSystemVersion` en
  `com.apple.migration.plist`, CommCenter fuera de `launchd.plist`,
  `BKNoWatchdogs` + excepción de watchdog para Setup y el esqueleto
  `/private/var/containers`. `herolte-flash-root.py --diff` reescribe solo
  los bloques de 64 MiB que cambiaron.
- **Root ajustado** (22 bloques reescritos con `--diff`, imagen
  `6e77614c…c156`): sin migración ni CommCenter. **Primer SwapEnd de
  backboardd** (`sel 5`, registro de 1132 bytes), seguido de inmediato por
  `display off` (el driver hace el fundido a negro y no presenta), luego
  `display on`; SpringBoard crea sus primeras IOSurfaces (ids 7–10). Muestreo
  a los ~6 min: SpringBoard en `GSEventRunModal` (UI arrancada), backboardd
  rasterizando en 4 hilos (`CA::OGL::SW::image_sampler`). Siguiente BOOT
  (`a3bce0c3…0758`): `a5fb_keep_on=1 a5fb_noconsole=1 a5fb_perf=1`.
- **0209 (volcado de registros de swap):** los dos primeros swaps son
  frames negros a propósito (`0x104=0x80000007`, todos los ids de superficie
  0, fondo `0x10c=0xff000000`); después no llega ningún swap con contenido.
  backboardd crea sus superficies de render como `0x62336138` ('b3a8', 2
  planos) de 1440x2560 y 1125x2436: la superficie por defecto (panel
  1440x2560) contradice el tamaño presentado (1125x2436).
- **🍎 iOS en pantalla (2026-10-22, boot `a2d97ebe…2239`, `sweetfb_native=1`):**
  con el tamaño real del panel, primero el logo de Apple y después la
  pantalla de configuración "Hola" (bonjour), con hora, batería e indicador
  de inicio de iPhone X. Táctil y botones sin driver todavía.
- `0210`: botones por GPIO ALIVE (`0x10580000`, del DTB del BOOT): Power
  GPA2 pin 4; Vol+/Vol-/Home GPA0 pines 3/4/5, activos en bajo, solo lectura
  con antirrebote (driver del A5 parametrizado). Táctil: `sec_ts` en 0x48 de
  HSI2C `0x14E10000`, IRQ GPA1 pin 0 (pendiente).
- **Botones en el teléfono (boot `8c9c5843…dd25`, 0210):** los cuatro se
  detectan y llegan a iOS (`SWEET HID: key 0xe9/0xea/0x40/0x30`). Power
  bloquea/desbloquea (con `a5fb_keep_on` la imagen queda congelada y al
  despertar vuelve "Hola"); Home y volumen no cambian nada en "Hola"
  (identidad iPhone X: se pasa deslizando hacia arriba).
- `0211`: driver táctil `bsd/dev/herolte_touch.c` (sec_ts en 0x48 de
  HSI2C `0x14E10000`, pines GPF1-0/1 función 2, INT GPA1-0 por sondeo cada
  5 ms; eventos `0x71` de 8 bytes, coordenadas 0..4095 escaladas a
  1440x2560) y `SweetHID` con la geometría del panel del S7. No toca las
  LDO32/33 del PMIC: si no responde, lo deja anotado.
- **0211 en el teléfono:** GPF1 `dat=0` y GPA1 `dat=0` (SDA/SCL e INT en
  bajo), el HSI2C táctil queda en `trans=0x2000c` con el primer byte en la
  FIFO: el sec_ts está sin alimentación después de S-Boot.
- **0212 en el teléfono:** el batcher del PMIC (`0x10550000`) completa el
  envío del registro, pero la lectura queda en `bat_state=0x200` con la FIFO
  RX vacía; no se encendió nada (la verificación de voltaje no llegó a
  correr). `0213`: init del bus como el probe de Linux (reset, master, código
  HS `0x0f`, HS|AUTO), tres formas de lectura (ext de Linux, 7 bits, con STOP)
  y volcado de registros en cada falla.
- **0213/0214:** ambos congelaron el teléfono (0213 tras resetear el HSI2C
  del PMIC; 0214 al arrancar el primer programa del batcher a ~56 s).
- **0215 (sondeo de solo lectura, arrancó bien):** PMU APM
  `config=0x1 status=0x10000 option=0` (M3 alimentado pero retenido, sin
  `ENABLE_APM`), mailbox `0x11240000` todo en 0, SRAM `0x11200000` con una
  tabla de vectores Cortex-M válida (`00004000 0000009d …`): S-Boot deja el
  firmware del APM cargado y el M3 detenido.
- `0216`: alimentación del táctil por el APM, como Linux con la pantalla
  encendida: liberar el M3 (`APM_CONFIG |= 1`, IRQ del mailbox a 0,
  `APM_OPTION |= ENABLE_APM`) y pedirle por mailbox lecturas/escrituras del
  S2MPS16 (LDO32/33). El batcher queda tras `herotouch=2`.
- **🖐 Táctil funcionando (0216, boot `626b33c7…2d1d`):** el APM responde
  por mailbox (S2MPS16 `id=0x2`, `L32CTRL=0x2e`, `L33CTRL=0x3c`;
  escrituras `0xee`/`0xfc` con respuesta `0xa`, readback correcto); con las
  LDO encendidas SDA/SCL suben (`GPF1 dat=0x3`), el sec_ts está en modo app
  (`0x20`, id `ac a5 52`, tx=15 rx=26) y los toques llegan a iOS
  (coordenadas 0..4095 confirmadas). Muy lento y el gesto de deslizar hacia
  arriba no se completa: lecturas I2C con `int=0x400` (NACK) justo después
  de apoyar el dedo pierden los eventos de movimiento.
- `0217`: reintentos de lectura como `sec_ts_i2c_read()` (hasta 10),
  contadores `moves/i2c_retries/lost_reads`; las pausas de 11 s solo con
  `herotouchslow=1` (el táctil queda listo a los pocos segundos).
- **0217 en el teléfono:** `moves=835 i2c_retries=6 lost_reads=0`: los
  movimientos llegan completos; el gesto en "Hola" no avanza porque Setup
  espera CoreTelephony/activación (igual que el donor).
- **🏠 Pantalla de inicio de iOS (2026-10-23):** imagen con Setup marcado
  como completo (imagen `035edb65…d60a`, flasheo diferencial de 155
  bloques): pantalla de inicio y Centro de control con el táctil. Sin apps
  (el staging excluía `/Applications`; dos iconos "Waiting…") y muy lento.
- `STAGE_ALL_APPS=1` en `ios-root` (todas las apps, ~215 MiB) y `0218` +
  ADT `cpu4..7`: los 4 Mongoose (MPIDR `0x0..0x3`) como lógicos 4–7 con
  `cpus=8`.
- **8 núcleos (ADT con `arm,cortex-a53` también en los Mongoose):**
  `8/8 CPUs initialized`; `0219` confirma contador común a los dos clusters
  (lag < 1 ms) y temporizadores que despiertan en los 8 (21 ms sobre 20).
  Los Mongoose reportan `midr=0x531f0011`, `ctr=0x84448005` (línea de
  I-cache de 128 B frente a 64 B en los A53).
- Con todas las apps se quedaba en el logo: `lsd` no terminaba de sembrar la
  base de LaunchServices porque MobileGestalt calculaba el UDID y esperaba
  para siempre `mac-address-bluetooth0` (no estaba en `/chosen`). Con MACs
  sintéticas en el ADT (`02:48:45:52:4f:0x`): **pantalla de inicio con
  iconos** (Fotos, App Store, Ajustes, Teléfono, Safari, Mensajes) y
  Ajustes > Información abre ("iPhone X", iOS 16.7.16, 21 GB).
- **Batería (0220/0221):** el fuel gauge es el del MAX77854 (`0x36` en
  HSI2C_1 `0x13650000`, pines GPD1-2/3; en HSI2C_0 no hay ACK). Lecturas
  reales en el teléfono: 100 %, 4339 mV, +114 mA con cargador; 4043 mV,
  −749 mA sin él; FULLCAP ≈ 2000 mAh. iOS muestra 100 % → 99 %.
- **Botón de encendido (0220):** con `a5fb_keep_on` se acepta el apagado de
  pantalla si llega < 3 s después de pulsar Power; bloquea y apaga la
  pantalla, y vuelve con la pantalla de bloqueo.
- `0222`: `sweetfb_scale=1` experimental, iOS dibuja 960x1706 y el kernel
  escala x1.5 (menos píxeles para el renderizado por CPU).
- **Más RAM (2026-10-23):** el segundo banco de DRAM (`0x880000000`,
  2 GiB, sin `reserved-memory`) para XNU con `mkadt_herolte.py --bank2`.
  Con 2 GiB el kernel ve la memoria pero entra en pánico en
  `kmem_range_init` (no entran sus rangos virtuales). Con
  `--bank2-mb 1024` arranca; los secundarios no llegaban (CPU_ON rc=0 con
  entrada > 4 GiB) y `0223` entra por un trampolín en `0x8fffe000`:
  **1 GiB y 8/8 núcleos**, pantalla de inicio "bastante más fluida, abre
  todo mucho más rápido".
- **0224:** copia a pantalla en paralelo (3 hilos ayudantes): de 10–20 ms a
  ~5 ms por cuadro, hasta 16 fps con movimiento. El log de `kmem` mostró
  por qué 2 GiB no entraba: el kernel (configuración macOS) reserva un
  compresor de 16 × RAM; `vm_compression_limit` lo limita. Con 2 GiB pasa
  ese punto pero entra en pánico en `write_pte_fast` (PTE no alineada,
  páginas de 16 KiB sobre 4 KiB). **Configuración actual: `--bank2
  --bank2-mb 1536`, `vm_compression_limit=98304`, `cpus=8`: 1.5 GiB,
  arranca.**
- **GPU Mali-T880 (0225/0226):** el dominio G3D y los relojes ya vienen
  encendidos de S-Boot; `GPU_ID=0x08802000` (Mali-T880 r2p0), 12 shader
  cores (`0xff0f`), 1 tiler, 1 L2. Con `herogpu=3`: L2, shader core 0 y
  tiler encendidos (ready `0x1`) y **primer trabajo en la GPU**:
  `WRITE_VALUE` en JS1/AS0 con `irq=0x2 mmu=0 exception=1 output=0`, sin
  fallos y sin afectar a iOS.
- `sweetfb_scale=1` (0222) muestra la imagen rayada y duplicada (las
  superficies a 960x1706 tienen otro formato): queda desactivado.
- Pendiente: nodo `/product` del ADT a partir del IPSW; root (UFS).
