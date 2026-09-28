/*
 * Spec: Injection.
 * This file is a comment guide. CMake does not compile it. Nothing injects it.
 *
 * When the boot script is real it runs in the page main world, after a
 * binding exists, on each page target and on every new document.
 * It defines window.Millennium.callServerMethod(pluginName, methodName, argsArray)
 * and returns a Promise. It does not fetch http://127.0.0.1.
 * Then it inserts only the pieces config.json enabled.
 *
 * Player: one script tag for
 *   https://steamloopback.host/steam-music-player/bootstrap.js?v=ASSET_REV
 * Skin: link and script tags from skin.json Patches, loaded from
 *   https://steamloopback.host/doomnet-skin/<file>
 * Skip TargetJs music-player-inject.js so the skin does not load the player.
 * Skip iframes (window !== window.top). Skip UseDefaultPatches.
 * Re-check document.title after load. Overlay titles arrive late.
 *
 * The page already borrows window.Millennium from opener, parent, or top.
 * Both the opener and the popup need the binding.
 */
