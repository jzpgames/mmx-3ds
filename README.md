# Mega Man X 3DS

Port no oficial de **Mega Man X (SNES)** para **New Nintendo 3DS / New 3DS XL / New 2DS XL**. No es un emulador: el juego está recompilado y corre nativo en la consola.

Necesitas tu propia ROM de **Mega Man X (USA) Rev 1 (v1.1)**. Este repositorio no incluye la ROM, ni código generado a partir de ella, ni música.

## Características
- **3D estereoscópico:** los fondos tienen profundidad en todas las fases, en las salas de jefes y en la selección de etapas. Con el deslizador abajo, la imagen es idéntica al original.
- **30 imágenes por segundo estables**, con la lógica del juego a 60 Hz.
- **Savestates:** 3 ranuras con miniatura, guardado y carga rápidos con SELECT + L/R.
- **Pantalla:** ancha nítida, 4:3 nítida o píxel exacto.
- **Interfaz táctil** en la pantalla inferior: estados guardados, pantalla, sonido y rendimiento.
- **Música MSU-1 opcional:** usa los packs estándar de Mega Man X, con volumen separado para música y efectos.

## Instalar
Descarga el zip desde **[Releases](../../releases)**. Adentro está el LEEME completo.

1. Instala el `.cia` con FBI, o copia `3ds/mmx-3ds/` a la SD y abre el `.3dsx` desde el Homebrew Launcher.
2. Copia tu ROM de **Mega Man X (USA) Rev 1 (v1.1)**, sin modificar, a `sdmc:/3ds/mmx-3ds/mmx.sfc`. Sirve con o sin header. Otras versiones (la 1.0 de EE. UU., la europea o la japonesa Rockman X) no funcionan: el juego verifica la ROM al iniciar.
   - CRC32 sin header: `DED53C64`
3. Para tener sonido, la SD debe tener el archivo `sdmc:/3ds/dspfirm.cdc`. Si otros homebrew ya te suenan, lo tienes. Si no, ejecuta una vez la app **DSP1** desde el Homebrew Launcher: copia ese archivo desde tu propia consola.
4. Opcional, para MSU-1: copia los `.pcm` de un pack a `sdmc:/3ds/mmx-3ds/`. No apliques ningún parche a la ROM.

## Controles
- **B:** saltar
- **Y:** disparar (mantén para cargar)
- **A:** dash
- **L / R:** cambiar arma
- **START:** menú
- **SELECT + L / R:** guardado y carga rápidos
- **L + R + START + SELECT:** salir

## Compilar
**Requisitos:**
- devkitPro con devkitARM r68 y libctru 2.7.1 (versiones exactas en `platform/3ds/sdk-versions.json`).
- Python 3.9+ y Rust (`rustup`), para generar el código desde la ROM.
- Para el `.cia`: `bannertool` y `makerom`.

```bash
git clone --recurse-submodules https://github.com/jzpgames/mmx-3ds.git
cd mmx-3ds
bash platform/3ds/apply-snesrecomp-patch.sh   # cambios del runtime para 3DS

cp "/ruta/a/Mega Man X (USA) (Rev 1).sfc" mmx.sfc   # no se sube (está en .gitignore)
bash tools/regen.sh usa --no-tests             # genera src/gen desde tu ROM

cd platform/3ds
make -f Makefile.game -j8                      # build-game/mmx-3ds.elf
./make-cia.sh                                  # dist/mmx-3ds.3dsx y dist/mmx-3ds.cia
```

Las herramientas de prueba del PC se compilan con `make -f Makefile.game HOST=1 <objetivo>` (por ejemplo `modecheck`, `profile` o `scene`).

## Estructura
- `platform/3ds/`: el port (bucle principal, compositor 3D, audio, MSU-1, interfaz, savestates).
- `src/`: núcleo del juego recompilado y renderizador.
- `recomp/`: configuración del recompilador.
- `snesrecomp/`: runtime y recompilador (submódulo). Se modifica con `platform/3ds/patches/snesrecomp-3ds.patch`.

## Créditos
- Port para New 3DS: Juanjo ([jzpgames](https://github.com/jzpgames)).
- Basado en [MegaManXSNESRecomp](https://github.com/mstan/MegaManXSNESRecomp) de mstan y en [snesrecomp](https://github.com/RetroPortingToolKit/snesrecomp).
- Anotaciones de código: [dizProjects](https://github.com/bogaa/dizProjects).
- Numeración MSU-1 compatible con el parche de DarkShock (Michaël Larouche). Los packs pertenecen a sus autores.

## Licencia
PolyForm Noncommercial 1.0.0, la misma del proyecto original (ver `LICENSE`). Uso libre y gratuito, sin fines comerciales.

Mega Man X © Capcom. Proyecto de fans, sin relación con Capcom ni con Nintendo.
