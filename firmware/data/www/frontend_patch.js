/* Frontend integration patch: WiFi static/DHCP, mDNS/about exposure, form polish hooks. */
!function () {
  "use strict";

  var app = window.App;
  if (!app) return;

  function el(id) {
    return document.getElementById(id);
  }

  function setValue(id, value) {
    var node = el(id);
    if (node && value !== undefined && value !== null) {
      node.value = String(value);
    }
  }

  function setChecked(id, value) {
    var node = el(id);
    if (node) node.checked = !!value;
  }

  function ipLooksValid(value) {
    var parts = String(value || "").split(".");
    if (parts.length !== 4) return false;
    for (var i = 0; i < parts.length; i++) {
      if (!/^\d{1,3}$/.test(parts[i])) return false;
      var octet = parseInt(parts[i], 10);
      if (octet < 0 || octet > 255) return false;
    }
    return true;
  }

  function status(id, ok, text) {
    var node = el(id);
    if (!node) return;
    node.textContent = text;
    node.className = "status-msg " + (ok ? "status-ok" : "status-err");
    node.style.display = "block";
  }

  function ensureWifiApSettingsFields() {
    if (el("wifi-ap-password") || !el("wifi-save-btn")) return;
    
    var apCard = document.createElement("div");
    apCard.className = "card";
    apCard.innerHTML = '<h3 class="card-title" data-i18n="wifi.apSettings">إعدادات نقطة الاتصال (AP)</h3>' +
      '<div class="form-group">' +
      '<label for="wifi-ap-ssid" data-i18n="wifi.apSsid">AP SSID (اسم نقطة الاتصال)</label>' +
      '<input type="text" id="wifi-ap-ssid" class="form-control" maxlength="32" placeholder="الافتراضي: ElmahdyRelay-XXXX">' +
      '</div>' +
      '<div class="form-group">' +
      '<label for="wifi-ap-password" data-i18n="wifi.apPassword">AP Password (كلمة مرور نقطة الاتصال)</label>' +
      '<div class="input-with-toggle">' +
      '<input type="password" id="wifi-ap-password" class="form-control" maxlength="63" autocomplete="new-password">' +
      '<button type="button" class="btn-show-password" aria-label="Show password" data-show-pw="wifi-ap-password">&#128065;</button>' +
      '</div></div>';
      
    el("wifi-save-btn").parentNode.insertBefore(apCard, el("wifi-save-btn"));
    
    var button = apCard.querySelector("[data-show-pw]");
    if (button) {
      button.addEventListener("click", function () {
        var input = el(button.getAttribute("data-show-pw"));
        if (!input) return;
        if (input.type === "password") {
          input.type = "text";
          button.textContent = "🙈";
        } else {
          input.type = "password";
          button.textContent = "👁";
        }
      });
    }
  }

  function ensureWifiPasswordModeFields() {
    if (el("wifi-password-mode") || !el("wifi-password")) return;

    var passwordInput = el("wifi-password");
    var passwordGroup = passwordInput.closest ? passwordInput.closest(".form-group") : passwordInput.parentNode;
    if (!passwordGroup || !passwordGroup.parentNode) return;

    var modeGroup = document.createElement("div");
    modeGroup.className = "form-group";
    modeGroup.id = "wifi-password-mode-group";
    modeGroup.innerHTML =
      '<label for="wifi-password-mode" data-i18n="wifi.passwordMode">WiFi password action</label>' +
      '<select id="wifi-password-mode" class="form-control">' +
      '<option value="keep" data-i18n="wifi.passwordKeep">Keep saved password</option>' +
      '<option value="new" data-i18n="wifi.passwordNew">Enter new password</option>' +
      '<option value="open" data-i18n="wifi.passwordOpen">Open network - no password</option>' +
      '</select>' +
      '<p class="field-hint" id="wifi-password-mode-hint"></p>';
    passwordGroup.parentNode.insertBefore(modeGroup, passwordGroup);

    var mode = el("wifi-password-mode");
    if (mode) mode.addEventListener("change", updateWifiPasswordMode);
  }

  function updateWifiPasswordMode() {
    var mode = el("wifi-password-mode");
    var password = el("wifi-password");
    var hint = el("wifi-password-mode-hint");
    var ssid = el("wifi-ssid") ? el("wifi-ssid").value.trim() : "";
    var sameSsid = ssid && ssid === (app._wifiSavedSsid || "");
    var hasSavedPassword = !!app._wifiHasPassword;
    var value = mode ? mode.value : "new";

    if (mode && value === "keep" && (!sameSsid || !hasSavedPassword)) {
      mode.value = hasSavedPassword && sameSsid ? "keep" : "new";
      value = mode.value;
    }

    if (password) {
      password.disabled = value !== "new";
      password.placeholder = value === "open"
        ? (app.strings["wifi.openNetwork"] || "No password will be saved")
        : (value === "keep"
            ? (app.strings["wifi.keepSavedPassword"] || "Saved password will be kept")
            : (app.strings["wifi.passwordPlaceholder"] || "Enter password"));
      if (value !== "new") password.value = "";
    }

    if (hint) {
      if (value === "keep") {
        hint.textContent = app.strings["wifi.passwordKeepHint"] || "Use this when changing IP/DHCP only.";
      } else if (value === "open") {
        hint.textContent = app.strings["wifi.passwordOpenHint"] || "Use this only for networks that do not require a password.";
      } else {
        hint.textContent = app.strings["wifi.passwordNewHint"] || "Use this when connecting to a new secured network or changing the password.";
      }
    }
  }

  function injectFormPolishCss() {
    if (el("frontend-form-polish")) return;
    var style = document.createElement("style");
    style.id = "frontend-form-polish";
    style.textContent =
      ":root{color-scheme:dark;--control-h:46px;--control-bg:#111a2d;--control-border:#405173;--control-border-strong:#6783b8;}" +
      "input[type=email],input[type=number],input[type=password],input[type=tel],input[type=text],input[type=url],select,textarea{min-height:var(--control-h);background-color:var(--control-bg)!important;border-color:var(--control-border)!important;color:var(--color-text-primary)!important;box-shadow:inset 0 1px 0 rgba(255,255,255,.03);}" +
      "select{height:var(--control-h);line-height:1.2;background-color:var(--control-bg)!important;color:var(--color-text-primary)!important;background-size:12px 8px;cursor:pointer;}" +
      "select option,select optgroup{background-color:#111a2d;color:#e8eaf6;}" +
      "select:focus,input:focus,textarea:focus{border-color:var(--control-border-strong)!important;box-shadow:0 0 0 3px rgba(103,131,184,.28)!important;}" +
      ".form-group-inline{gap:var(--sp-3);align-items:center;}" +
      ".form-group-inline>.form-control,.form-group-inline>select{flex:0 1 220px;min-width:156px;}" +
      ".relay-channel-fieldset{border:0;min-width:0;}" +
      ".relay-channel-legend{font-size:var(--font-size-sm);font-weight:700;color:var(--color-highlight);margin-bottom:var(--sp-3);}" +
      ".relay-channel-card{border-radius:var(--radius-md);}" +
      ".field-hint{margin:var(--sp-1) 0 0;color:var(--color-text-secondary);font-size:var(--font-size-xs);line-height:1.4;}" +
      ".relay-rename-btn{width:30px;height:30px;margin-inline-start:var(--sp-2);border-radius:var(--radius-md);border:1px solid var(--color-border);background:var(--color-bg-secondary);color:var(--color-text-secondary);font-size:15px;line-height:1;display:inline-flex;align-items:center;justify-content:center;transition:background-color var(--transition),color var(--transition),border-color var(--transition);}" +
      ".relay-rename-btn:hover,.relay-rename-btn:focus-visible{background:var(--color-card-hover);color:var(--color-text-primary);border-color:var(--color-border-focus);outline:0;}" +
      ".relay-rename-row{display:flex;gap:var(--sp-2);align-items:center;margin-top:calc(-1 * var(--sp-1));margin-bottom:var(--sp-2);}" +
      ".relay-rename-input{min-width:0;flex:1;}" +
      ".relay-rename-save{flex:0 0 auto;min-height:var(--control-h);}" +
      ".mqtt-topic-contract{display:flex;flex-direction:column;gap:var(--sp-2);}" +
      ".topic-row{display:grid;grid-template-columns:42px minmax(0,1fr);gap:var(--sp-2);align-items:start;padding:var(--sp-2);border:1px solid var(--color-border);border-radius:var(--radius-md);background:var(--color-bg-secondary);}" +
      ".topic-kind{font-size:var(--font-size-xs);font-weight:800;color:var(--color-highlight);text-transform:uppercase;}" +
      ".topic-row code{direction:ltr;text-align:left;white-space:normal;overflow-wrap:anywhere;color:var(--color-text-primary);font-size:var(--font-size-sm);}" +
      ".topic-payload{grid-column:2;font-size:var(--font-size-xs);color:var(--color-text-secondary);direction:ltr;text-align:left;}" +
      ".time-ampm{max-width:86px;flex:0 0 86px;}" +
      ".scene-quick-bar{display:flex;gap:var(--sp-2);overflow-x:auto;padding:var(--sp-1) 0 var(--sp-3);margin-top:var(--sp-2);}" +
      ".btn-scene{flex:0 0 auto;min-height:40px;padding:0 var(--sp-4);border-radius:var(--radius-pill);border:1px solid var(--color-border-focus);background:linear-gradient(135deg,var(--color-bg-accent),var(--color-card-hover));color:var(--color-text-primary);font-weight:700;box-shadow:var(--shadow-sm);}" +
      ".btn-scene:hover,.btn-scene:focus-visible{border-color:var(--color-highlight);background:var(--color-highlight);color:#fff;outline:0;}" +
      ".bulk-controls{display:grid;grid-template-columns:1fr 1fr;gap:var(--sp-3);margin-bottom:var(--sp-4);}" +
      ".toggle-switch .toggle-slider{position:absolute;inset:0;border-radius:var(--radius-pill);background:var(--toggle-off-bg);}" +
      ".toggle-switch .toggle-slider:before{content:'';position:absolute;top:3px;left:3px;width:var(--toggle-thumb);height:var(--toggle-thumb);border-radius:50%;background:var(--color-text-primary);box-shadow:var(--shadow-sm);transition:transform var(--transition);}" +
      ".toggle-switch input:checked+.toggle-slider{background:var(--toggle-on-bg);}" +
      ".toggle-switch input:checked+.toggle-slider:before{transform:translateX(calc(var(--toggle-width) - var(--toggle-thumb) - 6px));}" +
      "[dir=rtl] .toggle-switch input:checked+.toggle-slider:before{transform:translateX(calc(-1 * (var(--toggle-width) - var(--toggle-thumb) - 6px)));}" +
      "@media(max-width:520px){.form-group-inline{align-items:stretch;flex-direction:column!important}.form-group-inline>.form-control,.form-group-inline>select{width:100%;min-width:0;flex:auto}.bulk-controls{grid-template-columns:1fr}}" +
      "#restart-overlay{display:none;position:fixed;inset:0;z-index:9999;background:rgba(5,10,25,.92);backdrop-filter:blur(8px);flex-direction:column;align-items:center;justify-content:center;gap:24px;}" +
      "#restart-overlay.active{display:flex;}" +
      ".restart-spinner{width:56px;height:56px;border:4px solid rgba(103,131,184,.25);border-top-color:#6783b8;border-radius:50%;animation:rspin 0.8s linear infinite;}" +
      "@keyframes rspin{to{transform:rotate(360deg)}}" +
      ".restart-msg{color:#e8eaf6;font-size:1.15rem;font-weight:600;text-align:center;line-height:1.5;}" +
      ".restart-sub{color:#8ba0c8;font-size:0.85rem;text-align:center;}";
    document.head.appendChild(style);
  }

  function normalizeHostname(value) {
    return String(value || "elmahdyrelay")
      .toLowerCase()
      .replace(/[^a-z0-9-]/g, "-")
      .replace(/^-+|-+$/g, "")
      .substring(0, 20) || "elmahdyrelay";
  }

  app.loadWifiSettings = function () {
    ensureWifiPasswordModeFields();
    ensureWifiApSettingsFields();
    app.get("/api/config/wifi", function (cfg) {
      cfg = cfg || {};
      app._wifiSavedSsid = cfg.ssid || "";
      app._wifiHasPassword = !!cfg.hasPassword;
      setValue("wifi-ssid", cfg.ssid || "");
      setValue("wifi-static-ip", cfg.staticIp || "");
      setValue("wifi-gateway", cfg.gateway || "");
      setValue("wifi-subnet", cfg.subnet || "255.255.255.0");
      setValue("wifi-dns", cfg.dns || "8.8.8.8");
      setValue("wifi-ap-ssid", cfg.apSsid || "");
      setValue("wifi-ap-password", cfg.apPassword || "");
      setValue("wifi-password-mode", cfg.hasPassword ? "keep" : "open");
      setChecked("wifi-static-enabled", cfg.dhcp === false);
      app._toggleWifiStaticFields();
      updateWifiPasswordMode();
      if (app.applyStrings) app.applyStrings();
      app.loadWifiStatus();
    });
  };

  app.wifiSave = function () {
    var ssidNode = el("wifi-ssid");
    var passNode = el("wifi-password");
    var passwordModeNode = el("wifi-password-mode");
    var apSsidNode = el("wifi-ap-ssid");
    var apPassNode = el("wifi-ap-password");
    var useStatic = !!(el("wifi-static-enabled") && el("wifi-static-enabled").checked);
    var ssid = ssidNode ? ssidNode.value.trim() : "";
    var passwordMode = passwordModeNode ? passwordModeNode.value : "new";
    var apSsid = apSsidNode ? apSsidNode.value.trim() : "";
    var apPass = apPassNode ? apPassNode.value : "";
    
    // We no longer require STA SSID if they are just saving AP settings.
    // If they provided static IP fields, validate them.

    var staticIp = el("wifi-static-ip") ? el("wifi-static-ip").value.trim() : "";
    var gateway = el("wifi-gateway") ? el("wifi-gateway").value.trim() : "";
    var subnet = el("wifi-subnet") ? el("wifi-subnet").value.trim() : "255.255.255.0";
    var dns = el("wifi-dns") ? el("wifi-dns").value.trim() : "8.8.8.8";

    if (useStatic && (!ipLooksValid(staticIp) || !ipLooksValid(gateway) || !ipLooksValid(subnet) || !ipLooksValid(dns))) {
      status("wifi-status", false, app.strings["wifi.invalidIp"] || "Check the static IP fields.");
      return;
    }

    var payload = {
      ssid: ssid,
      passwordMode: passwordMode,
      password: passwordMode === "new" && passNode ? passNode.value : "",
      dhcp: !useStatic,
      staticIp: useStatic ? staticIp : "",
      gateway: useStatic ? gateway : "",
      subnet: useStatic ? subnet : "",
      dns: useStatic ? dns : "",
      apSsid: apSsid,
      apPassword: apPass
    };

    var button = el("wifi-save-btn");
    if (button) {
      button.disabled = true;
      button.textContent = app.strings["wifi.saving"] || "Saving...";
    }
    app.post("/api/config/wifi", payload, function (resp) {
      if (button) {
        button.disabled = false;
        button.textContent = app.strings["wifi.save"] || "Save";
      }
      status("wifi-status", !!(resp && resp.success),
        resp && resp.success
          ? (app.strings["wifi.saved"] || "Saved. Device will connect to the network.")
          : (app.strings["wifi.saveFailed"] || "Save failed. Please try again."));
    });
  };

  /* Ensure the restart-overlay element exists in the DOM. */
  function ensureRestartOverlay() {
    if (el("restart-overlay")) return;
    var overlay = document.createElement("div");
    overlay.id = "restart-overlay";
    overlay.setAttribute("role", "alertdialog");
    overlay.setAttribute("aria-live", "assertive");
    overlay.innerHTML =
      '<div class="restart-spinner"></div>' +
      '<p class="restart-msg" id="restart-overlay-msg">Restarting…</p>' +
      '<p class="restart-sub" id="restart-overlay-sub">Please wait while the device reboots.</p>';
    document.body.appendChild(overlay);
  }

  function showRestartOverlay() {
    ensureRestartOverlay();
    var overlay = el("restart-overlay");
    var msg = el("restart-overlay-msg");
    var sub = el("restart-overlay-sub");
    if (msg) msg.textContent = app.strings["system.restarting"] || "Restarting\u2026";
    if (sub) sub.textContent = app.strings["system.restartWait"] || "Please wait while the device reboots.";
    if (overlay) overlay.className = "active";
  }

  function hideRestartOverlay() {
    var overlay = el("restart-overlay");
    if (overlay) overlay.className = "";
  }

  function pollUntilAlive() {
    var attempts = 0;
    var MAX_ATTEMPTS = 30; // 60 s total
    var sub = el("restart-overlay-sub");
    function tryOnce() {
      var xhr = new XMLHttpRequest();
      xhr.open("GET", "/api/info", true);
      xhr.timeout = 3000;
      xhr.onreadystatechange = function () {
        if (xhr.readyState !== 4) return;
        if (xhr.status >= 200 && xhr.status < 300) {
          hideRestartOverlay();
          window.location.reload();
        } else {
          retry();
        }
      };
      xhr.ontimeout = retry;
      xhr.onerror = retry;
      xhr.send();
    }
    function retry() {
      attempts++;
      if (sub) {
        var remaining = Math.max(0, MAX_ATTEMPTS - attempts) * 2;
        sub.textContent = (app.strings["system.restartWait"] || "Please wait") + " (" + remaining + "s)";
      }
      if (attempts < MAX_ATTEMPTS) {
        setTimeout(tryOnce, 2000);
      } else {
        hideRestartOverlay();
        status("sys-status", false, app.strings["system.restartTimeout"] || "Device did not come back online — check connection.");
      }
    }
    // First poll after 3 s (boot takes at least that long)
    setTimeout(tryOnce, 3000);
  }

  app.saveSystemSettings = function () {
    var hostname = normalizeHostname(el("sys-hostname") ? el("sys-hostname").value : "elmahdyrelay");
    setValue("sys-hostname", hostname);
    var payload = {
      buzzerEnabled: !!(el("sys-buzzer-enabled") && el("sys-buzzer-enabled").checked),
      ledEnabled: !!(el("sys-led-enabled") && el("sys-led-enabled").checked),
      resetEnabled: !!(el("sys-reset-enabled") && el("sys-reset-enabled").checked),
      mdnsEnabled: !el("sys-mdns-enabled") || el("sys-mdns-enabled").checked,
      buzzerPin: el("sys-buzzer-pin") ? parseInt(el("sys-buzzer-pin").value, 10) : 13,
      resetPin: el("sys-reset-pin") ? parseInt(el("sys-reset-pin").value, 10) : 16,
      hostname: hostname,
      timezoneOffset: el("sys-timezone") ? parseInt(parseFloat(el("sys-timezone").value) * 60, 10) : 0
    };

    var saveBtn = el("sys-save-btn");
    if (saveBtn) { saveBtn.disabled = true; }

    app.post("/api/config/system", payload, function (resp) {
      if (saveBtn) { saveBtn.disabled = false; }
      if (resp && resp.restarting) {
        showRestartOverlay();
        pollUntilAlive();
      } else {
        status("sys-status", !!(resp && resp.success),
          resp && resp.success
            ? (app.strings["system.saved"] || "Saved")
            : (app.strings["system.saveFailed"] || "Save failed"));
        app.loadAboutInfo();
      }
    });
  };

  app.loadSystemSettings = function () {
    app.get("/api/config/system", function (t) {
      if (t) {
        var n = function (id, val) {
          var a = el(id);
          if (a) {
            if (a.type === "checkbox") a.checked = !!val;
            else if (val !== undefined) a.value = String(val);
          }
        };
        n("sys-buzzer-enabled", t.buzzerEnabled);
        n("sys-led-enabled", t.ledEnabled);
        n("sys-reset-enabled", t.resetEnabled);
        n("sys-mdns-enabled", t.mdnsEnabled === undefined ? true : t.mdnsEnabled);
        n("sys-buzzer-pin", t.buzzerPin);
        n("sys-reset-pin", t.resetPin);
        n("sys-hostname", t.hostname);
        n("sys-timezone", t.timezoneOffset !== undefined ? (t.timezoneOffset / 60) : 0);
      }
    });
  };

  app.loadAboutInfo = function () {
    app.get("/api/status", function (statusDoc) {
      app.get("/api/config/system", function (systemDoc) {
        statusDoc = statusDoc || {};
        systemDoc = systemDoc || {};
        statusDoc.hostname = statusDoc.hostname || systemDoc.hostname || "elmahdyrelay";
        if (!statusDoc.ip && statusDoc.wifi) statusDoc.ip = statusDoc.wifi.ip;
        if (!statusDoc.mac) statusDoc.mac = "--";
        if (!statusDoc.freeHeap && statusDoc.heap) statusDoc.freeHeap = statusDoc.heap;
        if (!statusDoc.mdns) {
          var host = normalizeHostname(statusDoc.hostname);
          statusDoc.mdns = {
            enabled: systemDoc.mdnsEnabled !== false,
            running: !!(statusDoc.wifi && statusDoc.wifi.ip),
            url: "http://" + host + ".local/"
          };
        }
        app.updateAboutInfo(statusDoc);
      });
    });
  };

  var originalUpdateAbout = app.updateAboutInfo;
  app.updateAboutInfo = function (doc) {
    if (originalUpdateAbout) originalUpdateAbout(doc);
    if (!doc) return;
    if (doc.hostname) app.setText("about-hostname", doc.hostname);
    if (doc.wifi && doc.wifi.ip) app.setText("about-ip", doc.wifi.ip);
    if (doc.heap && !doc.freeHeap) app.setText("about-heap", doc.heap + " bytes");
    if (doc.mdns) {
      var label = doc.mdns.enabled
        ? (doc.mdns.running ? (app.strings["about.mdnsRunning"] || "Running") : (app.strings["about.mdnsStopped"] || "Enabled, not running"))
        : (app.strings["about.mdnsDisabled"] || "Disabled");
      app.setText("about-mdns-status", label);
      var link = el("about-mdns-url");
      if (link) {
        link.textContent = doc.mdns.url || "--";
        if (doc.mdns.url) link.setAttribute("href", doc.mdns.url);
        else link.removeAttribute("href");
      }
    }
  };

  var originalLoadStatus = app.loadStatus;
  app.loadStatus = function () {
    if (originalLoadStatus) originalLoadStatus();
    if (app._activeTab === "about") app.loadAboutInfo();
  };

  function relayLabel(id, name) {
    return name || ((app.strings["relays.channel"] || "Channel") + " " + id);
  }

  function toast(ok, text) {
    var node = el("toast");
    if (!node) return;
    node.textContent = text;
    node.className = "toast toast-show " + (ok ? "toast-ok" : "toast-err");
    window.clearTimeout(app._relayRenameToastTimer);
    app._relayRenameToastTimer = window.setTimeout(function () {
      node.className = "toast";
    }, 2200);
  }

  function saveRelayName(id, name, done) {
    app.get("/api/config/relays", function (cfg) {
      cfg = cfg || {};
      var count = parseInt(cfg.channelCount, 10);
      if (isNaN(count) || count < id) count = id;
      if (count > 4) count = 4;
      var channels = cfg.channels && cfg.channels.length ? cfg.channels : [];
      var byId = {};
      for (var i = 0; i < channels.length; i++) {
        var channel = channels[i] || {};
        var channelId = parseInt(channel.id || (i + 1), 10);
        byId[channelId] = channel;
      }
      var out = [];
      for (var ch = 1; ch <= count; ch++) {
        var existing = byId[ch] || {};
        out.push({
          id: ch,
          pin: existing.pin !== undefined ? existing.pin : (app._defaultGpioPins[ch - 1] || 0),
          name: ch === id ? name : (existing.name || ""),
          powerOnState: existing.powerOnState || "last",
          pulseDuration: existing.pulseDuration || 0,
          interlockGroup: existing.interlockGroup || 0
        });
      }
      app.post("/api/config/relays", { channelCount: count, channels: out }, done);
    });
  }

  function enhanceRelayRename(relays) {
    if (!relays || !relays.length) return;
    for (var i = 0; i < relays.length; i++) {
      (function (relay) {
        var id = relay.id;
        var card = el("relay-card-" + id);
        if (!card || card.getAttribute("data-rename-ready") === "1") return;
        var header = card.querySelector(".relay-card-header");
        var nameNode = card.querySelector(".relay-name");
        if (!header || !nameNode) return;

        var edit = document.createElement("button");
        edit.type = "button";
        edit.className = "relay-rename-btn";
        edit.textContent = "\u270E";
        edit.title = app.strings["relays.rename"] || "Rename";
        edit.setAttribute("aria-label", edit.title);

        var row = document.createElement("div");
        row.className = "relay-rename-row";
        row.style.display = "none";

        var input = document.createElement("input");
        input.type = "text";
        input.className = "form-control relay-rename-input";
        input.id = "relay-rename-input-" + id;
        input.maxLength = 20;
        input.value = relay.name || "";
        input.placeholder = app.strings["relays.namePlaceholder"] || "Channel name";

        var save = document.createElement("button");
        save.type = "button";
        save.className = "btn-secondary btn-sm relay-rename-save";
        save.textContent = app.strings["relays.saveName"] || "Save";

        edit.addEventListener("click", function () {
          row.style.display = row.style.display === "none" ? "flex" : "none";
          input.value = nameNode.textContent === relayLabel(id, "") ? "" : nameNode.textContent;
          if (row.style.display !== "none") input.focus();
        });

        save.addEventListener("click", function () {
          var nextName = input.value.trim().substring(0, 20);
          save.disabled = true;
          saveRelayName(id, nextName, function (resp) {
            save.disabled = false;
            if (resp && resp.success) {
              nameNode.textContent = relayLabel(id, nextName);
              row.style.display = "none";
              toast(true, app.strings["relays.saved"] || "Saved");
              if (app._activeTab === "relays") app.loadRelaySettings();
            } else {
              toast(false, app.strings["relays.saveFailed"] || "Save failed");
            }
          });
        });

        input.addEventListener("keydown", function (event) {
          if (event.key === "Enter") save.click();
          if (event.key === "Escape") row.style.display = "none";
        });

        row.appendChild(input);
        row.appendChild(save);
        header.appendChild(edit);
        card.insertBefore(row, header.nextSibling);
        card.setAttribute("data-rename-ready", "1");
      })(relays[i]);
    }
  }

  var originalRenderRelayCards = app.renderRelayCards;
  app.renderRelayCards = function (relays) {
    if (originalRenderRelayCards) originalRenderRelayCards(relays);
    enhanceRelayRename(relays);
  };

  var originalUpdateRelayStates = app.updateRelayStates;
  app.updateRelayStates = function (relays) {
    if (originalUpdateRelayStates) originalUpdateRelayStates(relays);
    enhanceRelayRename(relays);
  };

  function removeFormGroup(id) {
    var node = el(id);
    if (!node) return;
    var group = node.closest ? node.closest(".form-group") : null;
    if (group && group.parentNode) group.parentNode.removeChild(group);
  }

  function mqttPrefix() {
    var node = el("mqtt-prefix");
    return node && node.value.trim() ? node.value.trim() : "elmahdy";
  }

  function topicLine(label, topic, payload) {
    return '<div class="topic-row"><span class="topic-kind">' + label + '</span>' +
      '<code>' + topic + '</code>' +
      (payload ? '<span class="topic-payload">' + payload + '</span>' : '') +
      '</div>';
  }

  function renderMqttContract() {
    var box = el("mqtt-topic-contract");
    if (!box) return;
    var prefix = mqttPrefix();
    box.innerHTML =
      '<h3 class="card-title">' + (app.strings["mqtt.topicContract"] || "Topic contract") + '</h3>' +
      topicLine("Sub", prefix + "/relay/{ch}/control", "ON / OFF / TOGGLE") +
      topicLine("Sub", prefix + "/relay/all/control", "ON / OFF") +
      topicLine("Sub", prefix + "/scene/{name}/control", "ON") +
      topicLine("Sub", prefix + "/timer/{id}/control", "CANCEL") +
      topicLine("Pub", prefix + "/relay/{ch}/status", "ON / OFF") +
      topicLine("Pub", prefix + "/system/status", "online / offline") +
      topicLine("Pub", prefix + "/system/info", "JSON") +
      topicLine("HA", "homeassistant/switch/{device_id}/relay_{ch}/config", "JSON");
  }

  function simplifyMqttPanel() {
    removeFormGroup("mqtt-username");
    removeFormGroup("mqtt-password");
    var enabled = el("mqtt-enabled");
    if (enabled) {
      enabled.checked = true;
      var enabledCard = enabled.closest ? enabled.closest(".card") : null;
      if (enabledCard) enabledCard.style.display = "none";
    }
    var fields = el("mqtt-fields");
    if (fields) fields.style.display = "block";
    if (!el("mqtt-topic-contract") && fields) {
      var contract = document.createElement("div");
      contract.id = "mqtt-topic-contract";
      contract.className = "card mqtt-topic-contract";
      fields.parentNode.insertBefore(contract, fields.nextSibling);
    }
    renderMqttContract();
  }

  app.loadMqttSettings = function () {
    simplifyMqttPanel();
    app.get("/api/config/mqtt", function (cfg) {
      cfg = cfg || {};
      setValue("mqtt-broker", cfg.broker || "broker.hivemq.com");
      setValue("mqtt-port", cfg.port || 1883);
      setValue("mqtt-prefix", cfg.prefix || "elmahdy");
      if (app.updateMqttConnectionStatus) {
        app.updateMqttConnectionStatus(cfg.connected);
      }
      renderMqttContract();
    });
  };

  app.saveMqttSettings = function () {
    var broker = el("mqtt-broker") ? el("mqtt-broker").value.trim() : "broker.hivemq.com";
    var port = el("mqtt-port") ? parseInt(el("mqtt-port").value, 10) : 1883;
    var prefix = mqttPrefix();
    app.post("/api/config/mqtt", {
      enabled: true,
      broker: broker || "broker.hivemq.com",
      port: port || 1883,
      prefix: prefix
    }, function (resp) {
      status("mqtt-status", !!(resp && resp.success),
        resp && resp.success ? (app.strings["mqtt.saved"] || "Saved") : (app.strings["mqtt.saveFailed"] || "Save failed"));
      renderMqttContract();
    });
  };

  app.updateMqttConnectionStatus = function (mqtt) {
    var connected = typeof mqtt === "object" ? !!mqtt.connected : !!mqtt;
    var node = el("mqtt-connection-status");
    if (node) {
      node.textContent = connected ? (app.strings["mqtt.connected"] || "Connected") : (app.strings["mqtt.disconnected"] || "Disconnected");
      node.className = connected ? "badge badge-ok" : "badge badge-error";
    }
  };

  var originalUpdateSystemInfo = app.updateSystemInfo;
  app.updateSystemInfo = function (doc) {
    if (originalUpdateSystemInfo) originalUpdateSystemInfo(doc);
    if (!doc) return;
    
    // Fix MQTT Badge on Dashboard (doc.mqtt is boolean)
    if (doc.mqtt !== undefined) {
      var isMqttConnected = !!doc.mqtt;
      app.updateMqttConnectionStatus(isMqttConnected);
      
      var mqttStatus = el("mqtt-status");
      var mqttItem = el("mqtt-item");
      if (mqttStatus) {
        mqttStatus.textContent = isMqttConnected ? (app.strings["mqtt.connected"] || "متصل") : (app.strings["mqtt.disconnected"] || "غير متصل");
        mqttStatus.className = "";
      }
      if (mqttItem) {
        mqttItem.className = "info-item " + (isMqttConnected ? "mqtt-on" : "mqtt-off");
      }
    }
    
    var timeNode = el("time-display");
    if (timeNode && doc.time) {
      timeNode.textContent = doc.ntpSynced ? doc.time : "--:--";
      var timeItem = timeNode.parentNode;
      if (!doc.ntpSynced) {
        if (timeItem && timeItem.classList) timeItem.classList.add("warn-item");
        timeNode.title = app.strings["system.timeNotSynced"] || "Time not synced";
      } else {
        if (timeItem && timeItem.classList) timeItem.classList.remove("warn-item");
        timeNode.title = "";
      }
    }
  };

  function to24Hour(hour12, ampm) {
    var h = parseInt(hour12, 10);
    if (isNaN(h) || h < 1) h = 12;
    if (h > 12) h = 12;
    if (ampm === "AM") return h === 12 ? 0 : h;
    return h === 12 ? 12 : h + 12;
  }

  function to12Hour(hour24) {
    var h = parseInt(hour24, 10);
    if (isNaN(h) || h < 0) h = 0;
    h = h % 24;
    return { hour: h % 12 || 12, ampm: h >= 12 ? "PM" : "AM" };
  }

  function installAmPmTime(hourId, minuteId, ampmId) {
    var hour = el(hourId);
    var minute = el(minuteId);
    if (!hour || el(ampmId)) return;
    var converted = to12Hour(hour.value);
    hour.min = "1";
    hour.max = "12";
    hour.value = String(converted.hour);
    hour.setAttribute("inputmode", "numeric");
    hour.addEventListener("blur", function() {
      var v = parseInt(hour.value, 10);
      if (isNaN(v) || v < 1) hour.value = "12";
      else if (v > 12) hour.value = "12";
    });
    if (minute) {
      minute.min = "0";
      minute.max = "59";
      minute.setAttribute("inputmode", "numeric");
      minute.addEventListener("blur", function() {
        var v = parseInt(minute.value, 10);
        if (isNaN(v) || v < 0) minute.value = "0";
        else if (v > 59) minute.value = "59";
      });
    }
    var ampm = document.createElement("select");
    ampm.id = ampmId;
    ampm.className = "form-control time-ampm";
    ampm.innerHTML = '<option value="AM">AM</option><option value="PM">PM</option>';
    ampm.value = converted.ampm;
    hour.parentNode.appendChild(ampm);
  }

  var originalAddScheduledTimer = app.addScheduledTimer;
  app.addScheduledTimer = function () {
    installAmPmTime("scheduled-hour", "scheduled-minute", "scheduled-ampm");
    var hour = el("scheduled-hour");
    var ampm = el("scheduled-ampm");
    if (hour && ampm) hour.value = String(to24Hour(hour.value, ampm.value));
    if (originalAddScheduledTimer) originalAddScheduledTimer();
    setTimeout(function () {
      if (hour && ampm) {
        var converted = to12Hour(hour.value);
        hour.value = String(converted.hour);
        ampm.value = converted.ampm;
      }
    }, 0);
  };

  var originalSaveScene = app.saveScene;
  app.saveScene = function () {
    installAmPmTime("scene-sched-hour", "scene-sched-minute", "scene-sched-ampm");
    var hour = el("scene-sched-hour");
    var ampm = el("scene-sched-ampm");
    if (hour && ampm) hour.value = String(to24Hour(hour.value, ampm.value));
    if (originalSaveScene) originalSaveScene();
    setTimeout(function () {
      if (hour && ampm) {
        var converted = to12Hour(hour.value);
        hour.value = String(converted.hour);
        ampm.value = converted.ampm;
      }
    }, 0);
  };

  var originalRenderSceneButtons = app.renderSceneButtons;
  app.renderSceneButtons = function (scenes) {
    if (originalRenderSceneButtons) originalRenderSceneButtons(scenes);
    var bar = el("scene-buttons");
    if (!bar) return;
    var buttons = bar.querySelectorAll(".btn-scene");
    for (var i = 0; i < buttons.length; i++) {
      buttons[i].setAttribute("title", app.strings["scene.activate"] || "Activate");
    }
  };

  var originalSaveMqttSettings = app.saveMqttSettings;
  app.saveMqttSettings = function() {
    var enabled = el("mqtt-enabled");
    var broker = el("mqtt-broker");
    var port = el("mqtt-port");
    var username = el("mqtt-username");
    var password = el("mqtt-password");
    var prefix = el("mqtt-prefix");

    var payload = {
      enabled: !!enabled && enabled.checked,
      broker: broker ? broker.value.trim() : "",
      port: port ? parseInt(port.value, 10) : 1883,
      username: username ? username.value.trim() : "",
      password: password ? password.value : "",
      prefix: prefix ? prefix.value.trim() : "elmahdy"
    };

    app.post("/api/config/mqtt", payload, function(res) {
      var statusEl = el("mqtt-status");
      if (!statusEl) return;

      if (res && res.success) {
        statusEl.textContent = app.strings["mqtt.saved"] || "Saved successfully";
        statusEl.className = "status-msg status-ok";
        statusEl.style.display = "block";

        if (res.restarting) {
          showRestartOverlay();
          pollUntilAlive();
        }
      } else {
        statusEl.textContent = app.strings["mqtt.saveFailed"] || "Failed to save settings";
        statusEl.className = "status-msg status-err";
        statusEl.style.display = "block";
      }
    });
  };

  function injectPremiumDesign() {
    if (el("premium-design")) return;
    var style = document.createElement("style");
    style.id = "premium-design";
    style.textContent = 
      ":root {" +
      "  --color-bg: #0b0f19;" +
      "  --color-bg-secondary: #131a29;" +
      "  --color-card: rgba(22, 30, 46, 0.65);" +
      "  --color-border: rgba(64, 81, 115, 0.4);" +
      "  --color-border-focus: #5d87ff;" +
      "  --color-highlight: #5d87ff;" +
      "  --color-highlight-dk: #496ee2;" +
      "  --color-success: #13de8f;" +
      "  --color-success-bg: rgba(19, 222, 143, 0.1);" +
      "  --color-error: #ff5b5b;" +
      "  --color-error-bg: rgba(255, 91, 91, 0.1);" +
      "  --radius-md: 12px;" +
      "  --radius-lg: 16px;" +
      "  --radius-xl: 24px;" +
      "  --shadow-sm: 0 4px 12px rgba(0,0,0,0.15);" +
      "  --shadow-md: 0 8px 24px rgba(0,0,0,0.2);" +
      "  --shadow-lg: 0 16px 40px rgba(0,0,0,0.3);" +
      "}" +
      "body {" +
      "  font-family: 'Inter', -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif;" +
      "  background: radial-gradient(circle at 50% 0%, #15223e 0%, var(--color-bg) 60%);" +
      "  background-attachment: fixed;" +
      "  color: #f1f5f9;" +
      "}" +
      ".app-header {" +
      "  background: rgba(11, 15, 25, 0.7);" +
      "  backdrop-filter: blur(12px);" +
      "  border-bottom: 1px solid var(--color-border);" +
      "  box-shadow: 0 4px 20px rgba(0,0,0,0.2);" +
      "}" +
      ".tab-nav {" +
      "  background: rgba(19, 26, 41, 0.8);" +
      "  backdrop-filter: blur(10px);" +
      "  border-bottom: 1px solid var(--color-border);" +
      "}" +
      ".tab-btn {" +
      "  color: #94a3b8;" +
      "  transition: all 0.3s cubic-bezier(0.4, 0, 0.2, 1);" +
      "}" +
      ".tab-btn:hover {" +
      "  color: #fff;" +
      "  background: rgba(255, 255, 255, 0.05);" +
      "}" +
      ".tab-btn.active {" +
      "  color: var(--color-highlight);" +
      "  background: transparent;" +
      "}" +
      ".tab-btn.active::after {" +
      "  content: '';" +
      "  position: absolute;" +
      "  bottom: 0; left: 0; width: 100%; height: 3px;" +
      "  background: var(--color-highlight);" +
      "  border-radius: 3px 3px 0 0;" +
      "  box-shadow: 0 -2px 10px rgba(93, 135, 255, 0.5);" +
      "}" +
      ".card {" +
      "  background: var(--color-card);" +
      "  backdrop-filter: blur(8px);" +
      "  border: 1px solid var(--color-border);" +
      "  box-shadow: var(--shadow-sm);" +
      "  transition: transform 0.2s ease, box-shadow 0.2s ease;" +
      "}" +
      ".card:hover {" +
      "  box-shadow: var(--shadow-md);" +
      "  border-color: rgba(93, 135, 255, 0.3);" +
      "}" +
      ".btn-primary {" +
      "  background: linear-gradient(135deg, var(--color-highlight) 0%, var(--color-highlight-dk) 100%);" +
      "  box-shadow: 0 4px 12px rgba(93, 135, 255, 0.3);" +
      "  border: none;" +
      "  font-weight: 600;" +
      "  letter-spacing: 0.5px;" +
      "  transition: transform 0.2s, box-shadow 0.2s;" +
      "}" +
      ".btn-primary:hover {" +
      "  transform: translateY(-2px);" +
      "  box-shadow: 0 6px 16px rgba(93, 135, 255, 0.4);" +
      "}" +
      ".btn-secondary {" +
      "  background: rgba(255, 255, 255, 0.05);" +
      "  border: 1px solid var(--color-border);" +
      "  backdrop-filter: blur(4px);" +
      "  transition: all 0.2s;" +
      "}" +
      ".btn-secondary:hover {" +
      "  background: rgba(255, 255, 255, 0.1);" +
      "  border-color: var(--color-highlight);" +
      "}" +
      ".btn-danger {" +
      "  background: linear-gradient(135deg, var(--color-error) 0%, #d63031 100%);" +
      "  box-shadow: 0 4px 12px rgba(255, 91, 91, 0.3);" +
      "  border: none;" +
      "}" +
      ".form-control, select {" +
      "  background: rgba(0, 0, 0, 0.2) !important;" +
      "  border-radius: 8px;" +
      "  transition: all 0.3s ease;" +
      "}" +
      ".form-control:focus, select:focus {" +
      "  box-shadow: 0 0 0 3px rgba(93, 135, 255, 0.15) !important;" +
      "  border-color: var(--color-highlight) !important;" +
      "  background: rgba(0, 0, 0, 0.3) !important;" +
      "}" +
      ".relay-card {" +
      "  background: linear-gradient(145deg, rgba(22, 30, 46, 0.8) 0%, rgba(16, 22, 35, 0.9) 100%);" +
      "  border: 1px solid var(--color-border);" +
      "  box-shadow: inset 0 1px 0 rgba(255, 255, 255, 0.05), var(--shadow-sm);" +
      "  transition: all 0.3s cubic-bezier(0.4, 0, 0.2, 1);" +
      "}" +
      ".relay-card:hover {" +
      "  transform: translateY(-3px);" +
      "  box-shadow: var(--shadow-md);" +
      "  border-color: rgba(93, 135, 255, 0.4);" +
      "}" +
      ".toggle-switch input:checked + .toggle-slider, .toggle-switch input:checked + .toggle-track {" +
      "  background: var(--color-highlight);" +
      "  box-shadow: 0 0 12px rgba(93, 135, 255, 0.5);" +
      "}" +
      ".system-info-bar {" +
      "  background: rgba(19, 26, 41, 0.6);" +
      "  backdrop-filter: blur(8px);" +
      "  border: 1px solid var(--color-border);" +
      "  border-radius: var(--radius-pill);" +
      "  padding: var(--sp-2) var(--sp-4);" +
      "  box-shadow: var(--shadow-sm);" +
      "}";
    document.head.appendChild(style);
  }

  function injectProfessionalUiCss() {
    if (el("professional-ui")) return;
    var style = document.createElement("style");
    style.id = "professional-ui";
    style.textContent =
      ":root{" +
      "  color-scheme:dark;" +
      "  --color-bg-primary:#0e1218;" +
      "  --color-bg:#0e1218;" +
      "  --color-bg-secondary:#151b24;" +
      "  --color-bg-accent:#1d2632;" +
      "  --color-card:#171f2a;" +
      "  --color-card-hover:#1b2531;" +
      "  --color-border:#303b4a;" +
      "  --color-border-focus:#7aa2d6;" +
      "  --color-highlight:#7aa2d6;" +
      "  --color-highlight-dk:#5f86b7;" +
      "  --color-success:#35c486;" +
      "  --color-success-bg:rgba(53,196,134,.12);" +
      "  --color-error:#e75f5f;" +
      "  --color-error-bg:rgba(231,95,95,.12);" +
      "  --color-warn:#d6a84c;" +
      "  --color-warn-bg:rgba(214,168,76,.12);" +
      "  --color-text-primary:#edf2f7;" +
      "  --color-text-secondary:#a5b2c2;" +
      "  --color-text-muted:#707d8f;" +
      "  --control-h:48px;" +
      "  --radius-sm:5px;" +
      "  --radius-md:7px;" +
      "  --radius-lg:8px;" +
      "  --radius-xl:10px;" +
      "  --shadow-sm:0 1px 2px rgba(0,0,0,.28);" +
      "  --shadow-md:0 10px 28px rgba(0,0,0,.24);" +
      "  --max-content-width:980px;" +
      "  --transition:160ms ease;" +
      "}" +
      "html{background:var(--color-bg-primary);}" +
      "body{background:var(--color-bg-primary)!important;color:var(--color-text-primary);font-family:system-ui,-apple-system,BlinkMacSystemFont,'Segoe UI',Tahoma,Arial,sans-serif;letter-spacing:0;}" +
      ".app-header{height:56px;background:#101722!important;border-bottom:1px solid var(--color-border)!important;box-shadow:none!important;backdrop-filter:none!important;}" +
      ".header-inner{max-width:var(--max-content-width);padding:0 18px;gap:14px;}" +
      ".app-title{font-size:16px;font-weight:750;letter-spacing:0;color:#f5f8fb;}" +
      ".header-device-name{max-width:210px;color:var(--color-text-secondary);}" +
      ".btn-icon{height:34px;min-width:42px;border-radius:var(--radius-md);background:#17202b;color:#dce6f1;border:1px solid var(--color-border);}" +
      ".btn-icon:hover,.btn-icon:focus-visible{background:#202b38;border-color:var(--color-border-focus);color:#fff;}" +
      ".tab-nav{position:sticky;top:56px;z-index:90;display:grid;grid-template-columns:repeat(auto-fit,minmax(76px,1fr));gap:1px;overflow:visible!important;background:#101722!important;border-bottom:1px solid var(--color-border)!important;padding:1px;box-shadow:0 8px 18px rgba(0,0,0,.16);backdrop-filter:none!important;direction:inherit!important;}" +
      ".tab-btn{position:relative;min-width:0!important;width:100%;height:44px!important;display:flex;flex-direction:row!important;align-items:center;justify-content:center;gap:6px;padding:0 6px!important;margin:0!important;border:0!important;border-radius:0;color:var(--color-text-secondary)!important;background:#141c27!important;font-size:12px!important;font-weight:650!important;line-height:1.15;white-space:normal!important;text-align:center;overflow:hidden;}" +
      ".tab-btn .tab-icon{font-size:12px;line-height:1;color:#788599;}" +
      ".tab-btn span:not(.tab-icon){min-width:0;overflow:hidden;text-overflow:ellipsis;}" +
      ".tab-btn:hover{background:#1a2430!important;color:#f0f5fa!important;}" +
      ".tab-btn.active{background:#223044!important;color:#fff!important;}" +
      ".tab-btn.active .tab-icon{color:var(--color-highlight);}" +
      ".tab-btn.active::after{content:'';position:absolute;inset-inline:10px;bottom:0;height:3px;border-radius:3px 3px 0 0;background:var(--color-highlight);box-shadow:none!important;}" +
      "@media(min-width:720px){.tab-nav{grid-template-columns:repeat(8,minmax(0,1fr));}.tab-btn{height:48px!important;font-size:13px!important;}}" +
      ".tab-container{max-width:var(--max-content-width);padding:20px 18px 30px;}" +
      ".tab-content{animation:fadeIn 140ms ease;}" +
      ".section-title{font-size:20px;font-weight:760;margin:0 0 16px;color:#f4f7fb;}" +
      ".card{background:var(--color-card)!important;border:1px solid var(--color-border)!important;border-radius:var(--radius-lg)!important;box-shadow:var(--shadow-sm)!important;padding:16px!important;margin-bottom:14px!important;backdrop-filter:none!important;transition:border-color var(--transition),background-color var(--transition)!important;}" +
      ".card:hover{transform:none!important;box-shadow:var(--shadow-sm)!important;border-color:#3a4655!important;background:var(--color-card-hover)!important;}" +
      ".card-title{font-size:15px;font-weight:750;color:#edf2f7;margin-bottom:12px;}" +
      ".card-header{min-height:36px;gap:12px;}" +
      ".card-row{gap:12px;padding:10px 0;border-color:rgba(255,255,255,.07);}" +
      ".card-label,.form-group label{font-size:13px;font-weight:650;color:var(--color-text-secondary);}" +
      ".card-value{font-size:13px;font-weight:650;min-width:0;overflow-wrap:anywhere;}" +
      ".system-info-bar{display:flex;flex-wrap:wrap;justify-content:space-between;align-items:center;gap:10px;background:rgba(19,26,41,.6)!important;backdrop-filter:blur(8px);border:1px solid var(--color-border)!important;border-radius:12px!important;padding:12px 16px!important;box-shadow:var(--shadow-sm)!important;margin-bottom:20px}" +
      ".info-item{display:flex;align-items:center;gap:6px;font-size:13px;font-weight:600;color:var(--color-text-primary);background:rgba(0,0,0,.2);padding:6px 12px;border-radius:8px;border:1px solid rgba(255,255,255,.05);transition:background .2s ease}" +
      ".info-item:hover{background:rgba(0,0,0,.35)}" +
      ".info-icon{font-size:15px;opacity:.85;margin-inline-end:2px}" +
      ".info-item.mqtt-on{color:var(--color-success);border-color:var(--color-success-bg);background:rgba(53,196,134,.05)}" +
      ".info-item.mqtt-off{color:var(--color-error);border-color:var(--color-error-bg);background:rgba(231,95,95,.05)}" +
      ".info-item.warn-item{color:var(--color-warn);border-color:var(--color-warn-bg);background:rgba(214,168,76,.05)}" +
      ".bulk-controls{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-bottom:14px;}" +
      ".relay-grid{gap:12px;}" +
      ".relay-card{background:#171f2a!important;border:1px solid var(--color-border)!important;border-radius:var(--radius-lg)!important;box-shadow:var(--shadow-sm)!important;gap:12px!important;}" +
      ".relay-card:hover{transform:none!important;border-color:#415066!important;}" +
      ".relay-card.relay-on{border-color:rgba(53,196,134,.72)!important;box-shadow:inset 0 0 0 1px rgba(53,196,134,.18)!important;}" +
      ".relay-card-header{gap:10px;align-items:center;}" +
      ".relay-name{min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;font-weight:750;}" +
      ".relay-badge{min-width:52px;height:26px;border-radius:var(--radius-md);letter-spacing:0;text-transform:none;}" +
      ".relay-btn-row{gap:8px;}" +
      ".rbtn{min-height:42px;border-width:1px!important;border-radius:var(--radius-md)!important;box-shadow:none!important;letter-spacing:0!important;}" +
      ".rbtn-on.rbtn-active,.rbtn-off.rbtn-active{box-shadow:none!important;}" +
      ".form-group{margin-bottom:14px;}" +
      ".form-group label{margin-bottom:6px;}" +
      ".form-group-inline{display:grid!important;grid-template-columns:minmax(0,1fr) auto;gap:12px;align-items:center!important;}" +
      ".form-group-inline>.form-control,.form-group-inline>select{width:min(220px,45vw);min-width:132px;flex:none!important;}" +
      "input[type=email],input[type=number],input[type=password],input[type=tel],input[type=text],input[type=url],select,textarea,.form-control{height:var(--control-h)!important;min-height:var(--control-h)!important;width:100%;padding:0 12px!important;background:#101722!important;border:1px solid var(--color-border)!important;border-radius:var(--radius-md)!important;color:#edf2f7!important;box-shadow:none!important;font-size:14px!important;line-height:var(--control-h)!important;outline:0!important;transition:border-color var(--transition),background-color var(--transition),box-shadow var(--transition)!important;}" +
      "textarea{height:auto!important;line-height:1.45!important;padding-top:10px!important;padding-bottom:10px!important;}" +
      "input:disabled,select:disabled,textarea:disabled{opacity:.62;background:#121820!important;cursor:not-allowed;}" +
      "input::placeholder,textarea::placeholder{color:#667386;}" +
      "select{appearance:none!important;-webkit-appearance:none!important;cursor:pointer;background-color:#101722!important;background-image:linear-gradient(45deg,transparent 50%,#a5b2c2 50%),linear-gradient(135deg,#a5b2c2 50%,transparent 50%)!important;background-position:calc(100% - 18px) 20px,calc(100% - 12px) 20px!important;background-size:6px 6px,6px 6px!important;background-repeat:no-repeat!important;padding-right:34px!important;}" +
      "[dir=rtl] select{background-position:18px 20px,12px 20px!important;padding-left:34px!important;padding-right:12px!important;}" +
      "select option,select optgroup{background:#101722!important;color:#edf2f7!important;}" +
      "input:focus,select:focus,textarea:focus,.form-control:focus{border-color:var(--color-border-focus)!important;background:#121a25!important;box-shadow:0 0 0 3px rgba(122,162,214,.18)!important;}" +
      ".form-control-file{min-height:var(--control-h)!important;padding:10px 12px!important;background:#101722!important;border:1px dashed var(--color-border)!important;border-radius:var(--radius-md)!important;color:var(--color-text-secondary)!important;font-size:13px!important;}" +
      ".input-with-toggle input{padding-inline-end:42px!important;}" +
      ".btn-show-password{height:32px;width:32px;inset-inline-end:8px!important;inset-inline-start:auto!important;border:1px solid transparent;color:#8d9aac;}" +
      ".btn-show-password:hover{background:#1c2634;color:#fff;border-color:var(--color-border);}" +
      ".btn-primary,.btn-secondary,.btn-danger{min-height:44px;border-radius:var(--radius-md)!important;font-size:14px!important;font-weight:750!important;letter-spacing:0!important;box-shadow:none!important;transform:none!important;}" +
      ".btn-primary{background:#2f6fa5!important;border-color:#2f6fa5!important;color:#fff!important;}" +
      ".btn-primary:hover:not(:disabled){background:#3b7fb7!important;border-color:#3b7fb7!important;}" +
      ".btn-secondary{background:#1b2531!important;border-color:var(--color-border)!important;color:#edf2f7!important;backdrop-filter:none!important;}" +
      ".btn-secondary:hover:not(:disabled){background:#222e3d!important;border-color:var(--color-border-focus)!important;}" +
      ".btn-danger{background:#b94747!important;border-color:#b94747!important;color:#fff!important;}" +
      ".btn-danger:hover:not(:disabled){background:#c95555!important;border-color:#c95555!important;}" +
      ".btn-sm{min-height:36px;padding:0 12px!important;font-size:13px!important;}" +
      ".btn-block{margin-top:10px;}" +
      ".segmented-control{background:#101722!important;border:1px solid var(--color-border);border-radius:var(--radius-md);padding:3px;}" +
      ".seg-btn{min-height:38px;border-radius:5px;color:var(--color-text-secondary);}" +
      ".seg-btn.active{background:#2f6fa5!important;color:#fff;}" +
      ".network-list{gap:8px;max-height:320px;padding:0!important;}" +
      ".network-item,.timer-item,.scene-item,.topic-row,.scene-channel-row{background:#101722!important;border:1px solid var(--color-border)!important;border-radius:var(--radius-md)!important;}" +
      ".network-item:hover,.network-item.selected{background:#182230!important;border-color:var(--color-border-focus)!important;}" +
      ".duration-inputs,.time-inputs{align-items:end;}" +
      ".duration-num,.time-num{text-align:center;min-width:0;}" +
      ".time-ampm{max-width:90px!important;flex:0 0 90px!important;}" +
      ".day-selector{gap:8px;}" +
      ".day-chip span{height:36px;min-width:42px;border-radius:var(--radius-md);background:#101722;border-color:var(--color-border);}" +
      ".day-chip input:checked+span{background:#2f6fa5;border-color:#2f6fa5;color:#fff;}" +
      ".status-msg{border-radius:var(--radius-md);padding:10px 12px;font-size:13px;}" +
      ".badge{height:24px;border-radius:var(--radius-md);padding:0 8px;}" +
      ".danger-actions{display:grid;grid-template-columns:1fr 1fr;gap:10px;}" +
      ".about-card{align-items:flex-start;text-align:start;padding:18px!important;}" +
      ".about-logo{font-size:34px;line-height:1;color:var(--color-highlight);}" +
      ".about-product{font-size:20px;}" +
      ".app-footer{border-top:1px solid var(--color-border);background:#101722;margin-top:18px;padding:16px;color:var(--color-text-muted);}" +
      ".modal-box{border-radius:var(--radius-xl);background:#171f2a;border-color:var(--color-border);box-shadow:var(--shadow-md);}" +
      "@media(max-width:520px){.header-inner{padding:0 12px}.header-device-name{display:none}.tab-container{padding:14px 10px 24px}.tab-nav{top:56px;grid-template-columns:repeat(4,minmax(0,1fr));}.tab-btn{height:42px!important;font-size:11px!important;gap:4px;padding:0 3px!important;}.tab-btn .tab-icon{font-size:11px}.card{padding:12px!important}.section-title{font-size:18px;margin-bottom:12px}.form-group-inline{grid-template-columns:1fr auto!important}.danger-actions{grid-template-columns:1fr}.duration-inputs{gap:6px}.time-inputs{gap:6px}.bulk-controls{grid-template-columns:1fr 1fr!important;}}" +
      "@media(max-width:360px){.tab-btn span:not(.tab-icon){font-size:10px}.tab-btn .tab-icon{display:none}.bulk-controls{grid-template-columns:1fr!important;}}" +
      "[dir=rtl] .header-inner,[dir=rtl] .header-actions{flex-direction:row-reverse;}" +
      "[dir=rtl] .tab-btn{flex-direction:row-reverse!important;}" +
      "[dir=rtl] .card-header,[dir=rtl] .card-row,[dir=rtl] .network-item,[dir=rtl] .relay-card-header,[dir=rtl] .timer-item,[dir=rtl] .scene-item{flex-direction:row-reverse;}" +
      "[dir=rtl] input,[dir=rtl] textarea{text-align:right;}" +
      ".ltr-text,[dir=rtl] .ltr-text{direction:ltr;text-align:left;unicode-bidi:embed;}";
    document.head.appendChild(style);
  }

  // Override renderSceneList to display scheduled times in 12-hour AM/PM format
  var originalRenderSceneList = app.renderSceneList;
  app.renderSceneList = function(t) {
    var n = el("scene-list");
    if (n) {
      var a, s, i, o, r, l, sa, c = el("scene-count-badge");
      if (c) c.textContent = t.length + "/10";
      if (t.length) {
        n.innerHTML = "";
        for (a = 0; a < t.length; a++) {
          s = t[a];
          i = document.createElement("div");
          i.className = "scene-item";
          var si = document.createElement("div");
          si.className = "scene-item-info";
          o = document.createElement("span");
          o.className = "scene-name";
          o.textContent = s.name;
          var sp = document.createElement("span");
          sp.className = "scene-states-preview";
          var previewParts = [];
          if (s.states && "object" == typeof s.states) {
            var sk = Object.keys(s.states);
            for (var sd = 0; sd < sk.length; sd++) {
              previewParts.push("Ch" + sk[sd] + ":" + (app.strings["relay." + (s.states[sk[sd]] || "off")] || s.states[sk[sd]] || "off"));
            }
          }
          if (s.schedule && s.schedule.enabled) {
            var rpt = s.schedule.repeatMode || "daily";
            var rptLabel = app.strings["timer.repeat." + rpt] || app.strings["timer." + rpt] || rpt;
            var h = s.schedule.hour || 0;
            var min = s.schedule.minute || 0;
            var ampm = h >= 12 ? "PM" : "AM";
            var h12 = h % 12;
            if (h12 === 0) h12 = 12;
            previewParts.push("⏰ " + app._pad2(h12) + ":" + app._pad2(min) + " " + ampm + " • " + rptLabel);
          }
          sp.textContent = previewParts.join(" | ");
          si.appendChild(o);
          si.appendChild(sp);
          sa = document.createElement("div");
          sa.className = "scene-item-actions";
          r = document.createElement("button");
          r.className = "scene-activate-btn";
          r.textContent = app.strings["scene.activate"] || "Activate";
          (function(t_name) {
            r.addEventListener("click", function() { app.activateScene(t_name); });
          })(s.name);
          l = document.createElement("button");
          l.className = "scene-delete-btn";
          l.textContent = "×";
          l.setAttribute("aria-label", "Delete scene");
          (function(t_name) {
            l.addEventListener("click", function() {
              if (confirm(app.strings["scene.confirmDelete"] || "Delete scene?")) {
                app.deleteScene(t_name);
              }
            });
          })(s.name);
          sa.appendChild(r);
          sa.appendChild(l);
          i.appendChild(si);
          i.appendChild(sa);
          n.appendChild(i);
        }
      } else {
        n.innerHTML = '<p class="empty-state" data-i18n="scene.noScenes">' + (app.strings["scene.noScenes"] || "No scenes") + "</p>";
      }
    }
  };

  document.addEventListener("DOMContentLoaded", function () {
    injectPremiumDesign();
    injectFormPolishCss();
    injectProfessionalUiCss();
    ensureWifiPasswordModeFields();
    ensureWifiApSettingsFields();
    var staticToggle = el("wifi-static-enabled");
    if (staticToggle) staticToggle.addEventListener("change", app._toggleWifiStaticFields);
    var ssidInput = el("wifi-ssid");
    if (ssidInput) ssidInput.addEventListener("input", updateWifiPasswordMode);
    var aboutTab = document.querySelector('[data-tab="about"]');
    if (aboutTab) aboutTab.addEventListener("click", function () {
      setTimeout(app.loadAboutInfo, 0);
    });
    simplifyMqttPanel();
    var prefix = el("mqtt-prefix");
    if (prefix) prefix.addEventListener("input", renderMqttContract);
    installAmPmTime("scheduled-hour", "scheduled-minute", "scheduled-ampm");
    installAmPmTime("scene-sched-hour", "scene-sched-minute", "scene-sched-ampm");
    
    // Load scenes on startup so dashboard buttons appear immediately
    setTimeout(app.loadScenes, 500);
  });
}();
