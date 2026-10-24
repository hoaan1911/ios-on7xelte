# Parches pendientes

No aplican sobre la serie normalizada de `xnu/patches/`: se generaron contra
estados de archivo que la serie publicada del A5 no reproduce. Hay que
reescribirlos a mano antes de moverlos de vuelta.

| Parche | Para HEROLTE |
| --- | --- |
| 0112, 0113, 0115 SMP | Más adelante (arrancar con `cpus=1`) |
| 0116 container-entitlement-trace | Diagnóstico, opcional |
| 0129–0134 G3D | Específicos de la Mali-T720 del A5; no aplican al S7 |

`0106` (AMFI) y `0107` (shared cache de 16 KiB en páginas de 4 KiB) se
reescribieron como `xnu/patches/0201-herolte-amfi-bridge.patch` y
`xnu/patches/0202-herolte-4k-unsigned-dyld-cache.patch`, que valen para
A5XELTE y HEROLTE.
