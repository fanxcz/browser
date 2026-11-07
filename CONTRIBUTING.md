# Contributing

1. Create a feature branch from `main`.
2. Keep changes focused and document new commands in `README.md`.
3. Build on Windows with Visual Studio and CMake:

   ```powershell
   cmake -S . -B build
   cmake --build build --config Release
   ```

4. Test HTTP, HTTPS, Russian page text, relative links, cache reload,
   downloads, cancellation, history, and bookmarks.
5. Open a pull request. The Windows GitHub Actions workflow must pass.

Do not commit `cache/`, `downloads/`, local browsing history, API tokens,
or other personal data.
