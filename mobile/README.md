# Vanta Mobile

**Write and run Vanta on your phone** — on-device, no server, works offline.

The Vanta interpreter is compiled to WebAssembly, so your code runs *inside your
phone's browser*. It installs to your home screen like a native app.

## Use it
Host this folder over HTTPS (or `localhost`) and open it on your phone:

```sh
cd mobile && python3 -m http.server 8095   # then open http://<your-ip>:8095
```

- Tap an example chip (Hello / Fib / Lambdas / Words / Map), or type your own.
- Tap **▶ Run**.
- **Add to Home Screen** (Share → Add to Home Screen on iOS; the **Install**
  button on Android/Chrome) — then it works fully offline.

It includes everything the native interpreter has: functions, lists/maps,
anonymous functions (`make x give x*2`), higher-order `map`/`keep`/`reduce`,
inline conditionals, word operators (`2 plus 3 times 4`), and more.
