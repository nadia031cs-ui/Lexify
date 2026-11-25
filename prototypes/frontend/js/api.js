
(function (global) {
    "use strict";

    var CONFIG = {
        
        API_BASE: global.LEXIFY_API_BASE || "/api",
        TOKEN_KEY: "lexify_token",
        USER_KEY: "lexify_user",
        LOGIN_PAGE: "login.html",
        HOME_PAGE: "index.html",
        ACCOUNT_PAGE: "account.html",
        LOGOUT_PAGE: "logout.html",
        DOWNLOAD_REQUIRES_LOGIN: true
    };

    function read(key) {
        try { return localStorage.getItem(key); } catch (e) { return null; }
    }
    function write(key, value) {
        try {
            if (value === null || value === undefined) localStorage.removeItem(key);
            else localStorage.setItem(key, value);
        } catch (e) { }
    }

    function getToken() { return read(CONFIG.TOKEN_KEY); }
    function getUser() {
        try { return JSON.parse(read(CONFIG.USER_KEY)) || null; } catch (e) { return null; }
    }
    function setUser(user) { write(CONFIG.USER_KEY, user ? JSON.stringify(user) : null); }
    function setSession(token, user) {
        write(CONFIG.TOKEN_KEY, token);
        setUser(user);
    }
    function clearSession() {
        write(CONFIG.TOKEN_KEY, null);
        write(CONFIG.USER_KEY, null);
    }
    function isLoggedIn() { return !!getToken(); }

    /* ---------- Page helpers ---------- */
    function currentPage() {
        return location.pathname.split("/").pop() || CONFIG.HOME_PAGE;
    }

    // Only allow simple same-folder page names (blocks open-redirect tricks).
    function nextUrl(fallback) {
        var next = new URLSearchParams(location.search).get("next");
        return next && /^[A-Za-z0-9_\-]+\.html$/.test(next) ? next : (fallback || CONFIG.HOME_PAGE);
    }

    function goToLogin() {
        location.replace(CONFIG.LOGIN_PAGE + "?next=" + encodeURIComponent(currentPage()));
    }

    function requireLogin() {
        if (isLoggedIn()) return true;
        goToLogin();
        return false;
    }

    /* ---------- HTTP ---------- */
    function ApiError(message, status, errors) {
        this.name = "ApiError";
        this.message = message;
        this.status = status || 0;
        this.errors = errors || null;   // { field: "message" } from the server
    }
    ApiError.prototype = Object.create(Error.prototype);

    function request(path, opts) {
        opts = opts || {};
        var headers = { "Accept": "application/json" };
        var token = getToken();
        var sendsToken = opts.auth !== false && !!token;

        if (opts.body !== undefined) headers["Content-Type"] = "application/json";
        if (sendsToken) headers["Authorization"] = "Bearer " + token;

        return fetch(CONFIG.API_BASE + path, {
            method: opts.method || "GET",
            headers: headers,
            body: opts.body !== undefined ? JSON.stringify(opts.body) : undefined
        }).catch(function () {
            throw new ApiError("Cannot reach the server. Check your connection and try again.", 0);
        }).then(function (res) {
            return res.text().then(function (text) {
                var data = null;
                try { data = text ? JSON.parse(text) : null; } catch (e) { }

                if (!res.ok) {
                    // Token expired / invalid -> back to login
                    if (res.status === 401 && sendsToken && !opts.noRedirect) {
                        clearSession();
                        goToLogin();
                    }
                    throw new ApiError(
                        (data && data.message) || ("Request failed (" + res.status + ")"),
                        res.status,
                        data && data.errors
                    );
                }
                return data;
            });
        });
    }

    /* ---------- API calls ---------- */
    function signup(name, email, password) {
        return request("/auth/signup", {
            method: "POST", auth: false,
            body: { name: name, email: email, password: password }
        });
    }

    function login(email, password) {
        return request("/auth/login", {
            method: "POST", auth: false,
            body: { email: email, password: password }
        }).then(function (data) {
            if (!data || !data.token) throw new ApiError("Unexpected response from server.", 0);
            setSession(data.token, data.user || null);
            return data.user || null;
        });
    }

    // Always clears the local session, even if the server call fails.
    function logout() {
        var call = isLoggedIn()
            ? request("/auth/logout", { method: "POST", noRedirect: true }).catch(function () { })
            : Promise.resolve();
        return call.then(clearSession);
    }

    function me() {
        return request("/user/me").then(function (data) {
            var user = (data && data.user) || data;
            setUser(user);
            return user;
        });
    }

    function updateProfile(profile) {
        return request("/user/profile", { method: "PUT", body: profile }).then(function (data) {
            var user = (data && data.user) || data;
            setUser(user);
            return user;
        });
    }

    function changePassword(currentPassword, newPassword) {
        return request("/user/password", {
            method: "PUT",
            body: { currentPassword: currentPassword, newPassword: newPassword }
        });
    }

    // Records a download (language, file name, code) for the logged-in user.
    function logDownload(info) {
        return request("/downloads", { method: "POST", body: info });
    }


    // Sends Lexify mini-language source (.mc) to the server compiler.
    // Resolves to { ok, tac, errors: [string], ast, symtab }.
    // Works for guests too; if a login token is stored it is sent so the server can count the run.
    function compile(code) {
        return request("/compile", { method: "POST", body: { code: String(code) } });
    }

    function asLines(v) {
        if (Array.isArray(v)) return v.join("\n");
        return v ? String(v) : "";
    }

    // Wires an editor page to the compiler.
    // opts: { runBtn, code (textarea), output (textarea), view (optional <select> with output|tac|ast|symtab) }
    function attachCompiler(opts) {
        var last = null;

        function show() {
            var out = opts.output;
            out.classList.remove("error");
            if (!last) return;
            if (!last.ok) {
                out.classList.add("error");
                out.value = asLines(last.errors) || "Compilation failed.";
                return;
            }
            var which = opts.view ? opts.view.value : "output";
            if (which === "output") {
                if (last.runtimeError) out.classList.add("error");
                var o = last.output == null ? "" : String(last.output);
                out.value = o ? o : "(program printed nothing)";
                return;
            }
            var text = which === "ast" ? asLines(last.ast)
                     : which === "symtab" ? asLines(last.symtab)
                     : asLines(last.tac);
            out.value = (text ? text.replace(/\n*$/, "\n") : "(empty)\n") + "[Compilation successful]";
        }

        opts.runBtn.addEventListener("click", function () {
            var code = opts.code.value;
            var out = opts.output;
            out.classList.remove("error");

            if (!code.trim()) {
                out.classList.add("error");
                out.value = "Nothing to run. Write some code first.";
                return;
            }

            opts.runBtn.disabled = true;
            out.value = "Compiling...";
            compile(code).then(function (result) {
                last = result || { ok: false, errors: ["Empty response from server."] };
                show();
            }).catch(function (err) {
                last = null;
                out.classList.add("error");
                out.value = err && err.message ? err.message : "Something went wrong.";
            }).then(function () {
                opts.runBtn.disabled = false;
            });
        });

        if (opts.view) opts.view.addEventListener("change", show);
    }

    /* ---------- Program shared between the compiler pages and download.html ---------- */
    var CURRENT_KEY = "lexify_current";
    var LANG_EXT = { "C": "c", "C++": "cpp", "Java": "java" };
    var MAX_SAVED_CODE = 500000;   // same limit the server uses for downloads

    // Safe base name + the right extension: ("my file.mc", "c") -> "my_file.c".
    function fileNameFor(name, ext, fallbackBase) {
        var base = String(name == null ? "" : name).trim()
            .replace(/\.(mc|c|cc|cpp|h|hpp|java|txt|pdf)$/i, "")
            .replace(/[^A-Za-z0-9_]+/g, "_")
            .slice(0, 60);
        if (!base.replace(/_/g, "")) base = fallbackBase || "main";
        if (ext === "java" && /^[0-9]/.test(base)) base = "_" + base;   // Java names can't start with a digit
        return base + "." + ext;
    }

    // Compiler pages call this while the user types; download.html reads it back.
    function saveCurrent(info) {
        if (!info || typeof info.code !== "string" || info.code.length > MAX_SAVED_CODE) return;
        write(CURRENT_KEY, JSON.stringify({
            language: info.language, fileName: info.fileName, page: info.page, code: info.code
        }));
    }

    // Returns { language, ext, base, fileName, page, code } or null if nothing valid is saved.
    function loadCurrent() {
        var d;
        try { d = JSON.parse(read(CURRENT_KEY)); } catch (e) { return null; }
        if (!d || typeof d.code !== "string" || !Object.prototype.hasOwnProperty.call(LANG_EXT, d.language)) return null;
        var m = /^([A-Za-z0-9_]+)\.(c|cpp|java)$/.exec(String(d.fileName));
        if (!m || m[2] !== LANG_EXT[d.language]) return null;
        return { language: d.language, ext: m[2], base: m[1], fileName: d.fileName, page: String(d.page || ""), code: d.code };
    }

    /* ---------- Show Login / Account / Logout depending on session ---------- */
    function applyAuthUI() {
        var logged = isLoggedIn();

        // Pages with a text menu: Login / Sign Up -> Account + Logout
        document.querySelectorAll("header nav").forEach(function (nav) {
            var hasLogoutLink = !!nav.querySelector('a[href="' + CONFIG.LOGOUT_PAGE + '"]');
            nav.querySelectorAll("a").forEach(function (a) {
                var href = a.getAttribute("href");
                if (!logged || (href !== "login.html" && href !== "signup.html")) return;

                a.setAttribute("href", CONFIG.ACCOUNT_PAGE);
                a.textContent = "Account";

                if (!hasLogoutLink) {
                    var out = document.createElement("a");
                    out.setAttribute("href", CONFIG.LOGOUT_PAGE);
                    out.textContent = "Logout";
                    a.insertAdjacentElement("afterend", out);
                    hasLogoutLink = true;
                }
            });
        });

        // Compiler pages (c / cpp / java): header "Account" button + sidebar Logout
        var account = document.querySelector("header nav a.account");
        if (account) {
            var label = account.querySelector("span:not(.avatar)");
            if (logged) {
                account.setAttribute("href", CONFIG.ACCOUNT_PAGE);
                var user = getUser();
                if (label && user && user.name) label.textContent = user.name.split(" ")[0];
            } else {
                account.setAttribute("href", CONFIG.LOGIN_PAGE);
                if (label) label.textContent = "Login";
            }
        }
        var sideLogout = document.getElementById("logout");
        if (sideLogout && !logged) sideLogout.style.display = "none";
    }

    if (document.readyState === "loading") {
        document.addEventListener("DOMContentLoaded", applyAuthUI);
    } else {
        applyAuthUI();
    }

    global.Lexify = {
        CONFIG: CONFIG,
        ApiError: ApiError,
        isLoggedIn: isLoggedIn, getToken: getToken, getUser: getUser,
        clearSession: clearSession, requireLogin: requireLogin, nextUrl: nextUrl,
        signup: signup, login: login, logout: logout, me: me,
        updateProfile: updateProfile, changePassword: changePassword, logDownload: logDownload,
        compile: compile, attachCompiler: attachCompiler,
        fileNameFor: fileNameFor, saveCurrent: saveCurrent, loadCurrent: loadCurrent, LANG_EXT: LANG_EXT
    };
})(window);
