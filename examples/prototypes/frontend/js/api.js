
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
        updateProfile: updateProfile, changePassword: changePassword, logDownload: logDownload
    };
})(window);
