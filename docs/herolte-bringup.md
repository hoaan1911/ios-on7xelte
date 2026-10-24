# herolte (SM-G930F, Exynos 8890): port de XNU / iOS 16

Base: la rama `codex/a5xelte-bringup` (Galaxy A5 2016, Exynos 7580), que ya
resolvió kernel de 4 KiB, Cortex-A53 sin LSE, GICv2, VA de 39 bits, loader
Samsung y la pantalla por copia de CPU. El S7 arranca en un Cortex-A53, así
que todo eso se reutiliza. Este documento separa **datos de fuentes** (DTS del
kernel de Samsung/Lineage y uniLoader) de **evidencia en el teléfono**, que
todavía no existe.

## Mapa de hardware (de fuentes, sin verificar en el teléfono)

Fuentes: `LineageOS/android_kernel_samsung_universal8890` rama `lineage-18.1`
(`exynos8890.dtsi`, `exynos8890-herolte_common.dtsi`,
`drivers/staging/samsung/sec_reboot.c`, `drivers/soc/samsung/exynos-reboot.c`)
y `ivoszbg/uniLoader` (`board/samsung/board-herolte.c`).

| Item | herolte | a5xelte (para comparar) |
| --- | --- | --- |
| CPU de arranque | Cortex-A53 "Apollo", **MPIDR 0x100..0x103** | A53, MPIDR 0..3 |
| Otros núcleos | 4× Samsung Mongoose M1, MPIDR 0..3 (dejar para después) | 4× A53, 0x100..0x103 |
| SMP | PSCI vía SMC, `cpu_on=0xC4000003` | PSCI 0.1 SMC |
| GIC | GIC-400 (v2): dist `0x11001000`, CPU `0x11002000` | `0x10c01000` / `0x10c02000` |
| Timer | 26 MHz, timer virtual PPI 11 → **INTID 27** | igual |
| RAM | `0x80000000` + `0x7e400000` y `0x880000000` + 2 GiB (4 GiB) | `0x40000000`, 2 GiB |
| Reservas conocidas | ESS `0x86000000` (bootargs), ECT `0xa0000000` (0x19000), ION `0xe0000000..0xeee00000` | — |
| Framebuffer de S-Boot | **`0xe2a00000`, 1440×2560 ARGB8888, stride 5760** (uniLoader) | `0x67000000`, 1080×1920 |
| DECON | DECON_F `0x13960000`; escribir `0x1010` en `+0x70` (HW trigger) para que el panel refresque | reprogramación completa DECON/DSIM |
| Almacenamiento | **UFS 2.0** en `0x155a0000` (`ufs-exynos.c`) | eMMC DW-MMC |
| Partición BOOT | `/dev/block/platform/155a0000.ufs/by-name/BOOT` | `mmcblk0p10` |
| PMU / reinicio | `0x105c0000`: INFORM2 `+0x808`=`0x12345678`, INFORM3 `+0x80c`=`0x12345674` (recovery), SWRESET `+0x400`=1 | igual en `0x10460000` |
| Touch | Samsung LSI `sec_ts` en `0x48`, HSI2C `0x14e10000`, IRQ GPA1-0 | Zinitix ZT7538 |
| PMIC | S2MPS16 en `0x66`, HSI2C `0x10550000` | S2MPU03 |
| Botones | Power GPA2-4, Vol− GPA0-4, Vol+ GPA0-3, **Home GPA0-5** (pinctrl `0x10580000`) | GPA2-5 Home |
| Teclas capacitivas | atrás/recientes por I²C GPIO en `0x20` (para después) | — |
| GPU | Mali-T880 MP12 (para después) | Mali-T720 |

Diferencia clave en almacenamiento: el driver eMMC del A5 (`0104`/`0108`) no
sirve. El driver UFS del donante sweet (`0028`, `0030`, `0034`, `0069`) habla
UFSHCI estándar; hay que adaptarlo a las particularidades de `ufs-exynos.c`
aprovechando que S-Boot ya dejó el enlace UFS inicializado.

## Etapas

1. **Probe de arranque (`shim/herolte/probe.*`)**: sin XNU. Confirma en
   pantalla EL, dirección del FDT, dirección de carga, MIDR/MPIDR, granulo de
   16 KiB, CNTFRQ y el rango del ramdisk. A los 60 s pide TWRP.
2. Loader `shim/herolte/xnu_boot.*` (adaptar `shim/a5xelte/xnu_boot.c`):
   ventana de RAM elegida a partir del resultado de la etapa 1 y consola en
   `0xe2a00000`.
3. Configuración `HEROLTE` de XNU: copia de los parches A5XELTE con GIC,
   framebuffer y marcador parametrizados; `cpus=1` y luego 4 A53.
4. Root iOS: driver UFS de solo lectura, luego escritura acotada a USERDATA.
5. Pantalla iOS (1440×2560), botones, touch `sec_ts`.

## Configuración HEROLTE de XNU

`MACHINE_CONFIG=HEROLTE` compila todo el código A5XELTE
(`-DARM64_BOARD_CONFIG_A5XELTE -DARM64_BOARD_CONFIG_HEROLTE`) con las
constantes de placa de `pexpert/pexpert/arm64/A5XELTE.h`: framebuffer
`0xe2a00000` 1440×2560 (stride 5760, 8 bloques de 2 MiB VA=PA), texto ×3,
577 ppi, marcador de entrada en `0x8ffff100`. Las teclas y el touch del A5
(direcciones del Exynos 7580) no se inician en HEROLTE. Todo está en
`xnu/patches/0200-herolte-board-target.patch` (numerado 0200 para no chocar
con los parches que siga publicando el A5).

### Serie de parches normalizada (2026-10-20)

La serie del A5 no aplicaba con el `git apply` simple de darwin-xnu-build:
`0094`/`0095` tenían cabeceras de hunk mal contadas, `0108` era `diff -U0`,
`0122` tenía un prefijo `xnu/` de más, y los scripts de staging de BearSSL y
HFS solo corrían con `SWEET`. Se reaplicó todo sobre `xnu-8796.141.3` con
staging, y cada parche se regeneró como diff con contexto completo: los 123
de `xnu/patches/` aplican en orden con `git apply` simple. Los que no
aplicaban están en `xnu/patches-pending/` (ver su README).

## Etapa 1: cómo probar

Compilar (`aarch64-none-elf-` o `aarch64-linux-gnu-`); genera
`out/probe.Image` y `out/xnu_boot.Image`:

```sh
CROSS=aarch64-none-elf- sh shim/herolte/build.sh
```

En TWRP (Terminal o `adb shell`), guardar el BOOT de stock y su tamaño:

```sh
dd if=/dev/block/platform/155a0000.ufs/by-name/BOOT of=/sdcard/boot-stock.img
blockdev --getsize64 /dev/block/platform/155a0000.ufs/by-name/BOOT
```

Copiar `boot-stock.img` a la PC (MTP de TWRP) y empaquetar:

```sh
python3 tools/herolte-pack-boot.py info boot-stock.img
python3 tools/herolte-pack-boot.py pack boot-stock.img shim/herolte/out/probe.Image probe-boot.img --partition-size 41943040
```

Flashear `probe-boot.img` desde TWRP (Instalar → Instalar imagen → Boot) y
reiniciar. Resultado esperado: texto blanco sobre el logo con los valores y
una cuenta regresiva, luego TWRP. Volver a stock instalando `boot-stock.img`
en Boot.

Si se cuelga sin texto: Vol− + Power ~7 s para forzar el reinicio y enseguida
Vol+ + Home + Power para TWRP. S-Boot y el modo Download no se tocan.

## Etapa 2: arrancar XNU (cuando Actions entregue el kernel)

El workflow `xnu` (macOS) sube el artefacto `xnu-kernel` con
`kernel.development.herolte`. Con eso, en la PC:

```sh
python3 tools/mkadt_herolte.py herolte.adt
python3 tools/mkpayload.py kernel.development.herolte herolte.adt payload.bin \
    --cmdline 'debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress'
python3 tools/herolte-pack-boot.py pack boot-stock.img shim/herolte/out/xnu_boot.Image \
    xnu-boot.img --append payload.bin --partition-size 41943040
```

El probe mostró que S-Boot pone el initrd en `0x89000000` y el FDT en
`0x8a000000` (no `0x82000000` como dice el DTB): solo 16 MiB, y el payload
pesa unos 20 MiB. Por eso `--append` lo pega al final de `xnu_boot.Image`
(slot del kernel, cargado en `0x800bc000`, que ya llevó un kernel de 38 MB).
`--ramdisk` queda como alternativa para payloads chicos. `xnu_boot` copia XNU a la ventana `0x90000000..0xb0000000`, escribe en
pantalla `ENTRY`/`BOOTARGS`/`JUMP TO XNU` y salta. Después XNU dibuja sus
bloques de color (entrada, MMU, FP, tunables, arm_init) y el texto
`XNU ENTRY` / `XNU MMU ON`, y luego la consola en C. Si el loader falla,
muestra el motivo 30 s y pide TWRP.
