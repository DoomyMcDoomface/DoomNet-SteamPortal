/* DoomNet page binding. Runs in the page main world. Defines
 * window.Millennium.callServerMethod and returns a Promise. The call goes
 * through the binding named doomnet. This script does not load anything from
 * a loopback address.
 *
 * Call ids start at 1000000 so they do not collide with calls a test posts
 * on the same bus.
 */
(function () {
    window.__doomnetTop = (window === window.top);
    if (window !== window.top && !window.__doomnetPlayerOnly) {
        // The store and community views are frames. The shell cannot reach
        // their documents. URL patches (webkit.css) still belong here.
        // The player binding stays on the top window.
        var frameConfig = window.__doomnetConfig;
        var framePatches = frameConfig && frameConfig.patches ? frameConfig.patches : [];
        var frameUrl = "";
        var frameTitle = "";
        try {
            frameUrl = String(location.href || "");
        } catch (err) {
            frameUrl = "";
        }
        try {
            frameTitle = document.title || "";
        } catch (err) {
            frameTitle = "";
        }
        var frameIndex;
        for (frameIndex = 0; frameIndex < framePatches.length; frameIndex++) {
            var framePatch = framePatches[frameIndex] || {};
            if (framePatch.useDefault) {
                continue;
            }
            var frameJs = framePatch.js ? String(framePatch.js) : "";
            var frameCss = framePatch.css ? String(framePatch.css) : "";
            if (frameJs.indexOf("music-player-inject.js") !== -1) {
                continue;
            }
            var frameMatch = framePatch.match ? String(framePatch.match) : "";
            var frameOk = !frameMatch;
            if (frameMatch) {
                try {
                    frameOk = new RegExp(frameMatch).test(frameTitle) || new RegExp(frameMatch).test(frameUrl);
                } catch (err) {
                    frameOk = false;
                }
            }
            if (!frameOk) {
                continue;
            }
            var frameParent = document.head || document.documentElement;
            if (!frameParent) {
                continue;
            }
            if (frameCss && !document.querySelector("link[href='" + frameCss + "']")) {
                var frameLink = document.createElement("link");
                frameLink.rel = "stylesheet";
                frameLink.href = frameCss;
                frameParent.appendChild(frameLink);
            }
            if (frameJs && !document.querySelector("script[src='" + frameJs + "']")) {
                var frameScript = document.createElement("script");
                frameScript.src = frameJs;
                frameScript.type = "module";
                frameParent.appendChild(frameScript);
            }
        }
        return;
    }
    // The binding can already exist when this script runs a second time, or when
    // a copy without the skin config ran first. Returning there skips the player
    // and the skin, so Steam reopens with neither. Install the binding once, then
    // always apply the config that is on this copy.
    var BOOTSTRAP_URL = "https://steamloopback.host/steam-music-player/bootstrap.js?v=20261112-version-1.18.1-skin-stays";
    var doomnetBindingReady = window.Millennium && typeof window.Millennium.callServerMethod === "function" && typeof window.__doomnetCall === "function";
    if (!doomnetBindingReady) {
    var pending = {};
    var nextId = 1000000;

    function rejectNow(message) {
        return Promise.reject(new Error(message));
    }

    function argOk(value) {
        var kind = typeof value;
        return kind === "string" || kind === "number" || kind === "boolean";
    }

    window.__doomnetSettle = function (id, ok, value) {
        var pair = pending[id];
        if (!pair) {
            return;
        }
        delete pending[id];
        if (ok) {
            pair.resolve(value);
        } else {
            pair.reject(new Error(typeof value === "string" ? value : "the host rejected the call"));
        }
    };

    function pack(value) {
        if (value === null || value === undefined) {
            return { k: "nil" };
        }
        var kind = typeof value;
        if (kind === "boolean") {
            return { k: "bool", v: value };
        }
        if (kind === "number") {
            return { k: "num", v: value };
        }
        if (kind === "string") {
            return { k: "str", v: value };
        }
        try {
            return { k: "json", v: JSON.stringify(value) };
        } catch (err) {
            return { k: "fail", e: String(err && err.message ? err.message : err) };
        }
    }

    window.__doomnetCall = function (method, params) {
        return new Promise(function (resolve) {
            function finish(packed) {
                resolve(packed);
            }
            try {
                var parts = String(method).split(".");
                var obj = window;
                var i;
                for (i = 0; i < parts.length - 1; i++) {
                    if (obj == null) {
                        finish({ k: "fail", e: "no such method on the page" });
                        return;
                    }
                    obj = obj[parts[i]];
                }
                var fn = obj && obj[parts[parts.length - 1]];
                if (typeof fn !== "function") {
                    finish({ k: "fail", e: "no such method on the page" });
                    return;
                }
                var list = (params && typeof params.length === "number") ? params : [];
                var result = fn.apply(obj, list);
                if (result && typeof result.then === "function") {
                    result.then(function (value) {
                        finish(pack(value));
                    }, function (err) {
                        finish({ k: "fail", e: String(err && err.message ? err.message : err) });
                    });
                    return;
                }
                finish(pack(result));
            } catch (err) {
                finish({ k: "fail", e: String(err && err.message ? err.message : err) });
            }
        });
    };

    window.Millennium = {
        callServerMethod: function (pluginName, methodName, argsArray) {
            if (typeof window.doomnet !== "function") {
                return rejectNow("DoomNet binding is not installed in this page");
            }
            if (argsArray == null) {
                argsArray = [];
            }
            if (typeof argsArray.length !== "number" || typeof argsArray !== "object") {
                return rejectNow("argsArray must be an array");
            }
            var i;
            for (i = 0; i < argsArray.length; i++) {
                if (!argOk(argsArray[i])) {
                    return rejectNow("Millennium's IPC can only handle [boolean, string, number]");
                }
            }
            var id = nextId++;
            return new Promise(function (resolve, reject) {
                pending[id] = { resolve: resolve, reject: reject };
                try {
                    window.doomnet(JSON.stringify({
                        id: id,
                        plugin: pluginName == null ? "" : String(pluginName),
                        method: methodName == null ? "" : String(methodName),
                        args: argsArray
                    }));
                } catch (err) {
                    delete pending[id];
                    reject(new Error(String(err && err.message ? err.message : err)));
                }
            });
        }
    };
    }

    function addScript(url, doc) {
        var soon = window.__doomnetScriptSoon || (window.__doomnetScriptSoon = {});
        if (soon[url]) {
            return;
        }
        soon[url] = true;
        var target = doc || document;
        if (!target || target.querySelector("script[src='" + url + "']")) {
            return;
        }
        // Modules run after the popup is patched, while Steam is still hidden.
        // A classic script with async forced on would block the library click.
        var tag = target.createElement("script");
        tag.src = url;
        tag.type = "module";
        (target.head || target.documentElement).appendChild(tag);
    }

    // client-neutralize.js watches the whole document and runs a tree query on
    // every mutation. That query lands inside the first library click and the
    // Home menu stays open. Skin code looks up MutationObserver later; Steam
    // already captured the native one. Defer only the skin observers.
    function quietObservers(win) {
        try {
            var Native = win.MutationObserver;
            if (!Native || Native.__doomnetQuiet) {
                return;
            }
            function Quiet(callback) {
                var pending = false;
                return new Native(function (records) {
                    if (pending) {
                        return;
                    }
                    pending = true;
                    var fire = function () {
                        pending = false;
                        callback(records);
                    };
                    if (win.requestIdleCallback) {
                        win.requestIdleCallback(fire, { timeout: 2000 });
                    } else {
                        win.setTimeout(fire, 2000);
                    }
                });
            }
            Quiet.__doomnetQuiet = true;
            win.MutationObserver = Quiet;
            win.__doomnetQuietObserver = true;
        } catch (err) {
        }
    }

    function wantsPlayer(title) {
        if (!title) {
            return false;
        }
        if (title === "Steam" || title === "Steam Games List" || title === "OverlayBrowser_Browser") {
            return true;
        }
        if (title.indexOf("SP Overlay:") === 0) {
            return true;
        }
        return /^OverlayTab\d+_Find$/.test(title);
    }

    function giveBinding(win) {
        try {
            if (win && !win.Millennium) {
                win.Millennium = window.Millennium;
            }
        } catch (err) {
        }
    }

    // Classic script, not a module. bootstrap.js is the only player file the
    // page inserts. music-player-inject.js is not used, so the player is not
    // loaded twice.
    // The player script runs in the Steam window. That window is not the page
    // that loaded this script, so it does not already have the Store record.
    function shareHostState(win) {
        if (!win || win === window) {
            return;
        }
        try {
            if (window.__doomnetStore) {
                win.__doomnetStore = window.__doomnetStore;
            }
            if (window.__doomnetPartyConfig) {
                win.__doomnetPartyConfig = window.__doomnetPartyConfig;
            }
            if (window.DoomNetStreamTunes) {
                win.DoomNetStreamTunes = window.DoomNetStreamTunes;
            }
        } catch (err) {
        }
    }

    function insertPlayer(win) {
        var cfg = window.__doomnetConfig;
        if (!cfg || !cfg.player || !win) {
            return;
        }
        shareHostState(win);
        giveBinding(win);
        var doc = null;
        try {
            doc = win.document;
        } catch (err) {
            return;
        }
        if (!doc || doc.querySelector("script[src='" + BOOTSTRAP_URL + "']")) {
            return;
        }
        var tag = doc.createElement("script");
        tag.src = BOOTSTRAP_URL;
        tag.async = false;
        (doc.head || doc.documentElement).appendChild(tag);
    }

    function addStyle(url, doc) {
        doc = doc || document;
        var tag = doc.createElement("link");
        tag.rel = "stylesheet";
        tag.href = url;
        (doc.head || doc.documentElement).appendChild(tag);
    }

    // Asset tags are inserted only when the host has published a config on this
    // page. A local proof page has no config, so it gets the binding and nothing else.
    // Skin patches match the document title, the page URL, or a class name.
    // music-player-inject.js is never inserted. UseDefaultPatches is not applied.
    var config = window.__doomnetConfig;
    if (window.__doomnetPlayerOnly) {
        // The store page blocks a steamloopback script tag. The host inlines
        // the player after this binding exists.
        return "binding";
    }
    if (config && window === window.top) {
        if (wantsPlayer(document.title || "")) {
            insertPlayer(window);
        }
        function already(doc, url) {
            if (!doc || !url) {
                return true;
            }
            try {
                return !!doc.querySelector("link[href='" + url + "'],script[src='" + url + "']");
            } catch (err) {
                return true;
            }
        }

        function insertPatch(doc, patch) {
            if (!doc || patch.useDefault) {
                return;
            }
            var js = patch.js ? String(patch.js) : "";
            var css = patch.css ? String(patch.css) : "";
            if (js.indexOf("music-player-inject.js") !== -1) {
                return;
            }
            if (css && !already(doc, css)) {
                addStyle(css, doc);
            }
            if (js && !already(doc, js)) {
                addScript(js, doc);
            }
        }

        function regexHit(text, pattern) {
            try {
                return new RegExp(pattern).test(text);
            } catch (err) {
                return false;
            }
        }

        function classNames(doc, extra) {
            var html = doc && doc.documentElement ? doc.documentElement.className : "";
            var body = doc && doc.body ? doc.body.className : "";
            var parts = (html + " " + body + " " + (extra || "")).split(/\s+/);
            var out = [];
            var i;
            for (i = 0; i < parts.length; i++) {
                if (parts[i]) {
                    out.push("." + parts[i]);
                }
            }
            return out;
        }

        function asList(value) {
            var out = [];
            if (!value) {
                return out;
            }
            if (typeof value.length === "number") {
                var n;
                for (n = 0; n < value.length && n < 200; n++) {
                    out.push(value[n]);
                }
                return out;
            }
            try {
                var iterate = value[Symbol.iterator];
                if (typeof iterate !== "function") {
                    return out;
                }
                var it = iterate.call(value);
                var step;
                while (!(step = it.next()).done && out.length < 200) {
                    out.push(step.value);
                }
            } catch (err) {
                return out;
            }
            return out;
        }

        function contexts() {
            var found = [{
                title: document.title || "",
                url: String(location.href || ""),
                doc: document,
                classes: classNames(document, "")
            }];
            var pops = [];
            try {
                if (window.g_PopupManager && typeof window.g_PopupManager.GetPopups === "function") {
                    pops = asList(window.g_PopupManager.GetPopups());
                }
            } catch (err) {
                pops = [];
            }
            var i;
            for (i = 0; i < pops.length; i++) {
                var popup = pops[i];
                if (!popup) {
                    continue;
                }
                var win = popup.window || (popup.m_popup && popup.m_popup.window);
                var doc = null;
                try {
                    doc = win && win.document;
                } catch (err) {
                    doc = null;
                }
                if (!doc) {
                    continue;
                }
                var title = "";
                var url = "";
                try {
                    title = String(popup.m_strTitle || popup.title || doc.title || "");
                } catch (err) {
                    title = "";
                }
                try {
                    url = String((win.location && win.location.href) || "");
                } catch (err) {
                    url = "";
                }
                var extra = "";
                try {
                    extra = String((popup.params && popup.params.body_class) || "") + " " + String((popup.params && popup.params.html_class) || "");
                } catch (err) {
                    extra = "";
                }
                found.push({ title: title, url: url, doc: doc, classes: classNames(doc, extra) });
            }
            return found;
        }

        function classHit(classes, pattern) {
            var i;
            for (i = 0; i < classes.length; i++) {
                if (classes[i].indexOf(pattern) !== -1) {
                    return true;
                }
            }
            return false;
        }

        // Millennium patches a popup once, when it is created, from the popup's
        // own title and body_class. It does not watch class mutations and it does
        // not read the live document class list. Doing that while the library
        // grid mounts forces style recalculation for every new game tile.
        function paramClasses(popup) {
            var body = "";
            var html = "";
            try {
                body = String((popup.params && popup.params.body_class) || "");
                html = String((popup.params && popup.params.html_class) || "");
            } catch (err) {
                body = "";
                html = "";
            }
            var parts = (body + " " + html).split(/\s+/);
            var out = [];
            var i;
            for (i = 0; i < parts.length; i++) {
                if (parts[i]) {
                    out.push("." + parts[i]);
                }
            }
            return out;
        }

        function liveClasses(doc) {
            var html = "";
            var body = "";
            try {
                html = doc.documentElement ? String(doc.documentElement.className || "") : "";
                body = doc.body ? String(doc.body.className || "") : "";
            } catch (err) {
                return [];
            }
            var parts = (html + " " + body).split(/\s+/);
            var out = [];
            var i;
            for (i = 0; i < parts.length; i++) {
                if (parts[i]) {
                    out.push("." + parts[i]);
                }
            }
            return out;
        }

        function labelText(el) {
            if (!el || el.childElementCount > 1) {
                return "";
            }
            return String(el.textContent || "").toLowerCase().replace(/[^a-z]/g, "");
        }

        function findLabel(doc, word) {
            var nodes = doc.querySelectorAll("button, a, [role='button'], [role='tab'], div, span");
            var i;
            var best = null;
            var bestTop = 100000;
            for (i = 0; i < nodes.length; i++) {
                if (labelText(nodes[i]) !== word) {
                    continue;
                }
                var rect = nodes[i].getBoundingClientRect();
                if (rect.width < 8 || rect.height < 8 || rect.top > bestTop) {
                    continue;
                }
                best = nodes[i];
                bestTop = rect.top;
            }
            return best;
        }

        function press(el) {
            if (!el) {
                return;
            }
            try {
                el.click();
            } catch (err) {
            }
        }

        function imageList(doc) {
            var imgs = doc.querySelectorAll("img");
            var out = [];
            var i;
            for (i = 0; i < imgs.length; i++) {
                out.push(imgs[i]);
            }
            return out;
        }

        function newCapsuleCount(doc, prior) {
            var imgs = doc.querySelectorAll("img");
            var ready = 0;
            var i;
            var j;
            for (i = 0; i < imgs.length; i++) {
                var known = false;
                for (j = 0; j < prior.length; j++) {
                    if (prior[j] === imgs[i]) {
                        known = true;
                        break;
                    }
                }
                if (known || imgs[i].naturalWidth <= 32) {
                    continue;
                }
                var rect = imgs[i].getBoundingClientRect();
                if (rect.width > 48) {
                    ready++;
                }
            }
            return ready;
        }

        // The first Library click builds the game grid and decodes capsule art.
        // Stay there until that first pass stops adding capsules, then return
        // to Store. The host keeps the window hidden until this finishes.
        function preloadLibrary(win) {
            if (!win || win.__doomnetLibraryWarm) {
                return;
            }
            win.__doomnetLibraryWarm = true;
            var doc = null;
            try {
                doc = win.document;
            } catch (err) {
                win.__doomnetLibraryReady = "skipped";
                return;
            }
            function finish(state) {
                var store = null;
                try {
                    store = findLabel(doc, "store");
                } catch (err) {
                    store = null;
                }
                press(store);
                try {
                    doc.dispatchEvent(new win.KeyboardEvent("keydown", { key: "Escape", keyCode: 27, which: 27, bubbles: true }));
                } catch (err2) {
                }
                win.__doomnetLibraryReady = state;
            }
            function waitForGrid(openedAt, prior, lastCount, stableSince) {
                var count = 0;
                try {
                    count = newCapsuleCount(doc, prior);
                } catch (err) {
                    count = 0;
                }
                var now = Date.now();
                if (count !== lastCount) {
                    lastCount = count;
                    stableSince = now;
                }
                var settled = count >= 6 && now - stableSince >= 2000;
                if (settled || now - openedAt > 45000) {
                    finish(count >= 6 ? "shown" : "skipped");
                    return;
                }
                win.setTimeout(function () { waitForGrid(openedAt, prior, lastCount, stableSince); }, 300);
            }
            function attempt(tries) {
                var lib = null;
                try {
                    lib = findLabel(doc, "library");
                } catch (err) {
                    lib = null;
                }
                if (!lib) {
                    if (tries > 40) {
                        win.__doomnetLibraryReady = "skipped";
                        return;
                    }
                    win.setTimeout(function () { attempt(tries + 1); }, 250);
                    return;
                }
                var prior = [];
                try {
                    prior = imageList(doc);
                } catch (err2) {
                    prior = [];
                }
                press(lib);
                win.setTimeout(function () {
                    var home = null;
                    try {
                        home = findLabel(doc, "home");
                    } catch (err) {
                        home = null;
                    }
                    press(home);
                    waitForGrid(Date.now(), prior, -1, Date.now());
                }, 600);
            }
            attempt(0);
        }

        function navLabel(el) {
            var text = "";
            try {
                text = el.innerText || el.textContent || "";
            } catch (err) {
                text = "";
            }
            return String(text).replace(/\s+/g, " ").trim().toUpperCase();
        }

        function findNavLabel(doc, word) {
            var nodes = doc.querySelectorAll("a, button, span, div");
            var i;
            for (i = 0; i < nodes.length; i++) {
                var el = nodes[i];
                if (el.id === "doomnet-steamtunes-tab") {
                    continue;
                }
                if (el.children && el.children.length > 3) {
                    continue;
                }
                if (labelIs(el, word)) {
                    return el;
                }
            }
            return null;
        }

        function navTabElement(labelEl) {
            var el = labelEl;
            var guard = 0;
            while (el && el.parentElement && guard < 8) {
                var parentText = navLabel(el.parentElement);
                if (parentText.indexOf("STORE") !== -1 && parentText.indexOf("LIBRARY") !== -1 && parentText.indexOf("COMMUNITY") !== -1) {
                    return el;
                }
                el = el.parentElement;
                guard++;
            }
            return labelEl;
        }

        function labelIs(el, word) {
            var text = navLabel(el);
            if (text === word) {
                return true;
            }
            if (text.indexOf(word) !== 0) {
                return false;
            }
            var rest = text.charAt(word.length);
            return rest === "" || rest === " " || rest < "A" || rest > "Z";
        }

        function activeNavExtras(storeTab, libraryTab, communityTab) {
            var tabs = [storeTab, libraryTab, communityTab];
            var counts = {};
            var i;
            var j;
            var names;
            for (i = 0; i < tabs.length; i++) {
                names = String(tabs[i].className || "").split(/\s+/);
                for (j = 0; j < names.length; j++) {
                    if (!names[j]) {
                        continue;
                    }
                    counts[names[j]] = (counts[names[j]] || 0) + 1;
                }
            }
            var extras = [];
            for (i = 0; i < tabs.length; i++) {
                names = String(tabs[i].className || "").split(/\s+/);
                for (j = 0; j < names.length; j++) {
                    if (names[j] && counts[names[j]] === 1 && extras.indexOf(names[j]) < 0) {
                        extras.push(names[j]);
                    }
                }
            }
            return extras;
        }

        function steamTunesOpen(doc) {
            var host = doc.getElementById("doomnet-steamtunes-host");
            return !!(host && host.classList.contains("doomnet-on"));
        }

        // Steam draws the store in a native browser view that owns the mouse
        // for its rectangle. Only the main window's views are held, and only
        // while the SteamTunes tab is open. Steam's own SetVisible/SetBounds
        // calls are remembered so they can be replayed when the tab closes.
        function boundsUsable(bounds) {
            return !!(bounds && bounds.w > 8 && bounds.h > 8);
        }

        function viewBounds(browser) {
            try {
                var now = typeof browser.GetBounds === "function" ? browser.GetBounds() : null;
                if (!now || typeof now.width !== "number" || typeof now.height !== "number") {
                    return null;
                }
                return { x: now.x || 0, y: now.y || 0, w: now.width, h: now.height };
            } catch (err) {
                return null;
            }
        }

        function rememberBounds(browser, bounds) {
            if (!boundsUsable(bounds)) {
                return;
            }
            var area = bounds.w * bounds.h;
            // Keep the largest rectangle Steam ever gave this view. A later,
            // smaller SetBounds while SteamTunes is open must not become the
            // size we restore, or only part of the page takes the mouse.
            if (!browser.__doomnetBestArea || area >= browser.__doomnetBestArea) {
                browser.__doomnetBestArea = area;
                browser.__doomnetWantBounds = bounds;
            }
        }

        // The page under the nav. Restoring an older, smaller rectangle leaves
        // a dead band where the mouse hits Steam's shell and nothing answers.
        function steamContentBox(doc) {
            var row = steamNavRow(doc);
            var top = 0;
            if (row && row.getBoundingClientRect) {
                top = Math.max(0, Math.round(row.getBoundingClientRect().bottom));
            }
            var root = doc.documentElement || doc.body;
            var w = (root && root.clientWidth) || 0;
            var h = (root && root.clientHeight) || 0;
            var box = { x: 0, y: top, w: w, h: h - top };
            return boundsUsable(box) ? box : null;
        }

        function holdSteamBrowser(doc, browser) {
            if (!browser || browser.__doomnetHeld) {
                return;
            }
            browser.__doomnetHeld = true;
            var show = browser.SetVisible.bind(browser);
            var place = browser.SetBounds.bind(browser);
            browser.__doomnetShow = show;
            browser.__doomnetPlace = place;
            browser.__doomnetWantVisible = null;
            browser.__doomnetWantBounds = null;
            rememberBounds(browser, viewBounds(browser));
            browser.SetVisible = function (on) {
                // Hiding the view while SteamTunes is open is Steam reacting
                // to that hide. Remembering it would leave the page invisible
                // after the tab closes, and the mouse would hit nothing.
                if (on || !steamTunesOpen(doc)) {
                    browser.__doomnetWantVisible = !!on;
                }
                if (steamTunesOpen(doc)) {
                    return show(false);
                }
                return show(on);
            };
            browser.SetBounds = function (x, y, w, h) {
                rememberBounds(browser, { x: x, y: y, w: w, h: h });
                if (steamTunesOpen(doc)) {
                    return undefined;
                }
                return place(x, y, w, h);
            };
        }

        function mainWindowBrowsers(win) {
            var out = [];
            var mgr = null;
            try {
                mgr = win.g_PopupManager || window.g_PopupManager;
            } catch (err) {
                mgr = null;
            }
            if (!mgr || typeof mgr.GetPopupForWindow !== "function") {
                return out;
            }
            var popup = null;
            try {
                popup = mgr.GetPopupForWindow(win);
            } catch (err2) {
                popup = null;
            }
            var views = popup && popup.childBrowserViews;
            if (!views || typeof views.length !== "number") {
                return out;
            }
            var i;
            for (i = 0; i < views.length; i++) {
                var view = views[i];
                if (view && typeof view.SetVisible === "function" && typeof view.SetBounds === "function") {
                    out.push(view);
                }
            }
            return out;
        }

        function suspendSteamPage(doc) {
            var win = doc && doc.defaultView;
            if (!win) {
                return;
            }
            var views = mainWindowBrowsers(win);
            var i;
            for (i = 0; i < views.length; i++) {
                var browser = views[i];
                holdSteamBrowser(doc, browser);
                rememberBounds(browser, viewBounds(browser));
                if (!boundsUsable(viewBounds(browser)) && !browser.__doomnetCollapsed) {
                    continue;
                }
                browser.__doomnetCollapsed = true;
                try {
                    browser.__doomnetShow(false);
                    browser.__doomnetPlace(0, 0, 0, 0);
                } catch (err) {
                }
            }
            doc.__doomnetBrowsers = views;
        }

        function resumeSteamPage(doc) {
            var views = (doc && doc.__doomnetBrowsers) || [];
            if (doc) {
                doc.__doomnetBrowsers = null;
            }
            var main = null;
            var mainArea = 0;
            var i;
            for (i = 0; i < views.length; i++) {
                var candidate = views[i];
                if (!candidate || !candidate.__doomnetCollapsed) {
                    continue;
                }
                var area = candidate.__doomnetBestArea || 0;
                if (area >= mainArea) {
                    main = candidate;
                    mainArea = area;
                }
            }
            var box = steamContentBox(doc);
            for (i = 0; i < views.length; i++) {
                var browser = views[i];
                if (!browser || !browser.__doomnetHeld || !browser.__doomnetCollapsed) {
                    continue;
                }
                browser.__doomnetCollapsed = false;
                try {
                    if (browser === main) {
                        var bounds = box || browser.__doomnetWantBounds;
                        if (boundsUsable(bounds)) {
                            browser.__doomnetPlace(bounds.x, bounds.y, bounds.w, bounds.h);
                        }
                        if (browser.__doomnetWantVisible !== false) {
                            browser.__doomnetShow(true);
                        }
                    } else {
                        // Leave the extra views collapsed. Showing every view
                        // again stacks them, and only some clicks reach Steam.
                        browser.__doomnetShow(false);
                    }
                } catch (err) {
                }
            }
        }

        function accentPaint(doc) {
            var probe = doc.createElement("span");
            probe.style.color = "var(--steam-accent-highlight)";
            var color = "";
            try {
                doc.body.appendChild(probe);
                color = doc.defaultView.getComputedStyle(probe).color || "";
            } catch (err) {
                color = "";
            }
            if (probe.parentNode) {
                probe.parentNode.removeChild(probe);
            }
            return color;
        }

        function restoreOtherNav(doc) {
            var tab = doc.getElementById("doomnet-steamtunes-tab");
            var rec = tab && tab.__doomnetReleased;
            if (!rec || !rec.el || !rec.names) {
                if (tab) {
                    tab.__doomnetReleased = null;
                }
                return;
            }
            var i;
            for (i = 0; i < rec.names.length; i++) {
                rec.el.classList.add(rec.names[i]);
            }
            tab.__doomnetReleased = null;
        }

        function releaseOtherNav(doc) {
            var tab = doc.getElementById("doomnet-steamtunes-tab");
            var row = steamNavRow(doc);
            restoreOtherNav(doc);
            if (!tab || !row) {
                return;
            }
            var accent = accentPaint(doc);
            if (!accent) {
                return;
            }
            var kids = row.children;
            var active = null;
            var i;
            var j;
            for (i = 0; i < kids.length && !active; i++) {
                if (kids[i] === tab) {
                    continue;
                }
                var labels = kids[i].querySelectorAll("span, a, div");
                for (j = 0; j < labels.length && j < 8; j++) {
                    var painted = "";
                    try {
                        painted = doc.defaultView.getComputedStyle(labels[j]).color || "";
                    } catch (err) {
                        painted = "";
                    }
                    if (painted === accent) {
                        active = kids[i];
                        break;
                    }
                }
            }
            if (!active) {
                return;
            }
            var names = String(active.className || "").split(/\s+/);
            var removed = [];
            for (i = 0; i < names.length; i++) {
                if (!names[i]) {
                    continue;
                }
                var alone = true;
                for (j = 0; j < kids.length; j++) {
                    if (kids[j] === active) {
                        continue;
                    }
                    if (String(kids[j].className || "").split(/\s+/).indexOf(names[i]) >= 0) {
                        alone = false;
                        break;
                    }
                }
                if (alone) {
                    active.classList.remove(names[i]);
                    removed.push(names[i]);
                }
            }
            tab.__doomnetReleased = { el: active, names: removed };
        }

        function hideSteamTunes(doc, win) {
            var host = doc.getElementById("doomnet-steamtunes-host");
            var tab = doc.getElementById("doomnet-steamtunes-tab");
            if (host) {
                host.classList.remove("doomnet-on");
            }
            resumeSteamPage(doc);
            restoreOtherNav(doc);
            if (tab) {
                var extras = tab.__doomnetExtras || [];
                var i;
                for (i = 0; i < extras.length; i++) {
                    tab.classList.remove(extras[i]);
                }
                tab.__doomnetExtras = [];
                tab.classList.remove("doomnet-tab-on");
            }
            if (typeof win.__doomnetHidePlayer === "function") {
                win.__doomnetHidePlayer();
            }
        }

        function showSteamTunes(doc, win) {
            var host = doc.getElementById("doomnet-steamtunes-host");
            var tab = doc.getElementById("doomnet-steamtunes-tab");
            if (!host || !tab) {
                return;
            }
            var row = steamNavRow(doc);
            if (row) {
                host.style.top = Math.max(0, Math.round(row.getBoundingClientRect().bottom)) + "px";
            }
            host.classList.add("doomnet-on");
            tab.classList.add("doomnet-tab-on");
            suspendSteamPage(doc);
            adoptSteamTunesPlayer(doc, win, host);
            var store = findNavLabel(doc, "STORE");
            var library = findNavLabel(doc, "LIBRARY");
            var community = findNavLabel(doc, "COMMUNITY");
            var extras = [];
            if (store && library && community) {
                extras = activeNavExtras(navTabElement(store), navTabElement(library), navTabElement(community));
            }
            var old = tab.__doomnetExtras || [];
            var i;
            for (i = 0; i < old.length; i++) {
                tab.classList.remove(old[i]);
            }
            for (i = 0; i < extras.length; i++) {
                tab.classList.add(extras[i]);
            }
            tab.__doomnetExtras = extras;
            releaseOtherNav(doc);
            function reveal() {
                if (typeof win.__doomnetShowPlayer === "function") {
                    win.__doomnetShowPlayer();
                }
                var panel = host.querySelector(".smp-panel");
                if (panel) {
                    panel.classList.remove("smp-hidden");
                }
            }
            reveal();
            win.requestAnimationFrame(function () {
                reveal();
                win.requestAnimationFrame(reveal);
            });
        }

        function adoptSteamTunesPlayer(doc, win, host) {
            var launcher = doc.querySelector(".smp-launcher");
            if (launcher) {
                launcher.remove();
            }
            var panel = doc.querySelector(".smp-panel");
            if (panel && !host.contains(panel)) {
                panel.classList.add("smp-tab-page");
                host.appendChild(panel);
            }
            if (typeof win.__doomnetShowPlayer !== "function") {
                win.__doomnetShowPlayer = function () {
                    var live = doc.getElementById("doomnet-steamtunes-host");
                    var node = live && live.querySelector(".smp-panel");
                    if (node) {
                        node.classList.remove("smp-hidden");
                    }
                };
                win.__doomnetHidePlayer = function () {
                    var live = doc.getElementById("doomnet-steamtunes-host");
                    var node = live && live.querySelector(".smp-panel");
                    if (node) {
                        node.classList.add("smp-hidden");
                    }
                };
            }
        }

        function steamNavRow(doc) {
            var community = findLabel(doc, "community");
            if (!community) {
                return null;
            }
            var outer = navTabElement(community);
            return outer.parentElement || outer;
        }

        function bindSteamTunesClicks(doc, win) {
            if (win.__doomnetTabClicks) {
                return;
            }
            win.__doomnetTabClicks = true;
            doc.addEventListener("click", function (event) {
                if (doc.__doomnetSwitching) {
                    return;
                }
                var tab = doc.getElementById("doomnet-steamtunes-tab");
                var host = doc.getElementById("doomnet-steamtunes-host");
                if (!tab || !host) {
                    return;
                }
                var target = event.target;
                if (!target) {
                    return;
                }
                if (target === tab || (tab.contains && tab.contains(target))) {
                    event.preventDefault();
                    event.stopPropagation();
                    showSteamTunes(doc, win);
                    return;
                }
                if (host.contains(target)) {
                    if (target.classList && target.classList.contains("smp-close")) {
                        hideSteamTunes(doc, win);
                    }
                    return;
                }
                var row = steamNavRow(doc);
                if (row && row.contains(target)) {
                    hideSteamTunes(doc, win);
                }
            }, true);
        }

        function parkTab(doc) {
            var tab = doc.__doomnetTabNode || doc.getElementById("doomnet-steamtunes-tab");
            var community = findLabel(doc, "community");
            if (!tab || !community) {
                return false;
            }
            var outer = navTabElement(community);
            var row = outer.parentElement;
            if (!row || row === doc.body || row === doc.documentElement) {
                return false;
            }
            tab.style.position = "relative";
            tab.style.left = "auto";
            tab.style.top = "auto";
            tab.style.height = "";
            tab.style.zIndex = "auto";
            if (row.lastElementChild !== tab) {
                row.appendChild(tab);
            }
            if (!row.__doomnetObserved) {
                row.__doomnetObserved = true;
                var observer = new doc.defaultView.MutationObserver(function () {
                    try {
                        parkTab(doc);
                    } catch (err) {
                    }
                });
                observer.observe(row, { childList: true });
            }
            return true;
        }

        function installSteamTunesTab(doc, win) {
            if (!doc || !win || doc.getElementById("doomnet-steamtunes-tab") || doc.__doomnetTabNode) {
                return !!doc && !!(doc.getElementById("doomnet-steamtunes-tab") || doc.__doomnetTabNode);
            }
            var store = findLabel(doc, "store");
            var library = findLabel(doc, "library");
            var community = findLabel(doc, "community");
            if (!store || !library || !community) {
                return false;
            }
            var storeRect = store.getBoundingClientRect();
            var libraryRect = library.getBoundingClientRect();
            var communityRect = community.getBoundingClientRect();
            if (storeRect.width < 8 || libraryRect.width < 8 || communityRect.width < 8) {
                return false;
            }
            if (Math.abs(storeRect.top - libraryRect.top) > 12 || Math.abs(libraryRect.top - communityRect.top) > 12) {
                return false;
            }
            if (!(storeRect.left < libraryRect.left && libraryRect.left < communityRect.left)) {
                return false;
            }
            var communityTab = navTabElement(community);
            if (!doc.getElementById("doomnet-steamtunes-style")) {
                var style = doc.createElement("style");
                style.id = "doomnet-steamtunes-style";
                style.textContent = "#doomnet-steamtunes-host{display:none;position:fixed;left:0;right:0;bottom:0;z-index:2147482000;background:#141414;}"
                    + "#doomnet-steamtunes-host.doomnet-on{display:block;pointer-events:auto;}"
                    + "#doomnet-steamtunes-host,#doomnet-steamtunes-host *{-webkit-app-region:no-drag !important;}"
                    + "#doomnet-steamtunes-host .smp-panel,#doomnet-steamtunes-host .smp-body,#doomnet-steamtunes-host .smp-library-pane,#doomnet-steamtunes-host button{pointer-events:auto !important;}"
                    + "#doomnet-steamtunes-host .smp-panel{position:absolute !important;inset:0 !important;left:0 !important;right:0 !important;top:0 !important;bottom:0 !important;width:100% !important;height:100% !important;max-width:none !important;max-height:none !important;border:0 !important;border-radius:0 !important;box-shadow:none !important;}"
                    + ".smp-launcher{display:none !important;}"
                    + "#doomnet-steamtunes-host .smp-panel{display:flex !important;flex-direction:column !important;}"
                    + "#doomnet-steamtunes-host .smp-body{flex:1 1 auto !important;min-height:0 !important;display:flex !important;flex-direction:column !important;}"
                    + "#doomnet-steamtunes-host .smp-view-library.active{display:flex !important;flex-direction:column !important;flex:1 1 auto !important;min-height:0 !important;overflow:hidden !important;}"
                    + "#doomnet-steamtunes-host .smp-library-pane{flex:1 1 auto !important;min-height:0 !important;overflow:auto !important;}"
                    + "#doomnet-steamtunes-tab{position:relative !important;left:auto !important;top:-4px !important;height:32px !important;min-height:32px !important;display:flex !important;align-items:center !important;align-self:center !important;box-sizing:border-box !important;margin:0 !important;z-index:auto !important;cursor:pointer;flex:0 0 auto !important;line-height:32px !important;}"
                    + "#doomnet-steamtunes-tab>*{display:flex !important;align-items:center !important;line-height:1 !important;}"
                    + "html body #doomnet-steamtunes-tab.doomnet-tab-on,html body #doomnet-steamtunes-tab.doomnet-tab-on *{color:var(--steam-accent-highlight,#c93939) !important;}"
                    + "html body #doomnet-steamtunes-tab.doomnet-tab-on::after{content:\"\";position:absolute;left:0;right:0;bottom:0;height:2px;background:var(--steam-accent-highlight,#c93939) !important;pointer-events:none;}";
                (doc.head || doc.documentElement).appendChild(style);
            }
            var host = doc.getElementById("doomnet-steamtunes-host");
            if (!host) {
                host = doc.createElement("div");
                host.id = "doomnet-steamtunes-host";
                (doc.body || doc.documentElement).appendChild(host);
            }
            win.__doomnetTabHost = host;
            var tab = doc.createElement(communityTab.tagName || "div");
            tab.id = "doomnet-steamtunes-tab";
            doc.__doomnetTabNode = tab;
            tab.className = communityTab.className || "";
            if (community !== communityTab) {
                var inner = doc.createElement(community.tagName || "span");
                inner.className = community.className || "";
                inner.textContent = "STEAMTUNES";
                tab.appendChild(inner);
            } else {
                tab.textContent = "STEAMTUNES";
            }
            tab.removeAttribute("href");
            if (host.classList.contains("doomnet-on")) {
                tab.classList.add("doomnet-tab-on");
            }
            if (!parkTab(doc)) {
                (doc.body || doc.documentElement).appendChild(tab);
            }
            return true;
        }

        function watchSteamTunesTab(win) {
            if (win.__doomnetTabWatch) {
                return;
            }
            win.__doomnetTabWatch = true;
            function tick() {
                var doc = null;
                try {
                    doc = win.document;
                } catch (err) {
                    return;
                }
                try {
                    bindSteamTunesClicks(doc, win);
                    installSteamTunesTab(doc, win);
                    parkTab(doc);
                    var host = doc.getElementById("doomnet-steamtunes-host");
                    if (host) {
                        adoptSteamTunesPlayer(doc, win, host);
                        if (host.classList.contains("doomnet-on")) {
                            var row = steamNavRow(doc);
                            if (row) {
                                host.style.top = Math.max(0, Math.round(row.getBoundingClientRect().bottom)) + "px";
                            }
                            suspendSteamPage(doc);
                        }
                    }
                } catch (err) {
                }
                win.setTimeout(tick, 250);
            }
            tick();
        }

        function patchWindow(win, title, url, classes) {
            if (!win || win.HAS_INJECTED_THEME) {
                return;
            }
            // A popup often exists before Steam has named it. Marking it done
            // at that moment skips the skin forever and the client stays default.
            if (!title) {
                return;
            }
            var doc = null;
            try {
                doc = win.document;
            } catch (err) {
                return;
            }
            if (!doc) {
                return;
            }
            // A skin tag that throws must not skip the player, and must not mark
            // this window finished before the player script is in the page.
            var skinDone = false;
            try {
                quietObservers(win);
                var all = classes.concat(liveClasses(doc));
                var patches = config.patches || [];
                var p;
                for (p = 0; p < patches.length; p++) {
                    var patch = patches[p] || {};
                    var match = patch.match ? String(patch.match) : "";
                    if (!match) {
                        insertPatch(doc, patch);
                        continue;
                    }
                    var ok = regexHit(title, match) || regexHit(url, match) || classHit(all, match);
                    if (!ok && match === "^Steam$") {
                        ok = regexHit(title, "^Steam Games List$");
                    }
                    if (ok) {
                        insertPatch(doc, patch);
                    }
                }
                skinDone = true;
            } catch (err) {
            }
            var playerDone = !wantsPlayer(title);
            try {
                if (title === "Steam" || title === "Steam Games List") {
                    shareHostState(win);
                    watchSteamTunesTab(win);
                    preloadLibrary(win);
                }
                if (wantsPlayer(title)) {
                    insertPlayer(win);
                }
                playerDone = true;
            } catch (err) {
            }
            if (skinDone && playerDone) {
                win.HAS_INJECTED_THEME = true;
            }
        }

        function scanPopups() {
            var pops = [];
            try {
                if (window.g_PopupManager && typeof window.g_PopupManager.GetPopups === "function") {
                    pops = asList(window.g_PopupManager.GetPopups());
                }
            } catch (err) {
                pops = [];
            }
            var i;
            for (i = 0; i < pops.length; i++) {
                var popup = pops[i];
                if (!popup) {
                    continue;
                }
                var win = popup.window || (popup.m_popup && popup.m_popup.window);
                if (!win || win.HAS_INJECTED_THEME) {
                    continue;
                }
                var title = "";
                var url = "";
                try {
                    title = String(popup.m_strTitle || popup.title || "");
                } catch (err) {
                    title = "";
                }
                if (!title) {
                    try {
                        title = String((win.document && win.document.title) || "");
                    } catch (err2) {
                        title = "";
                    }
                }
                try {
                    url = String((win.location && win.location.href) || "");
                } catch (err) {
                    url = "";
                }
                patchWindow(win, title, url, paramClasses(popup));
            }
        }

        scanPopups();
        window.setInterval(scanPopups, 250);
    }
})();
