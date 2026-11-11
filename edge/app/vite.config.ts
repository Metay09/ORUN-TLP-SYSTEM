import { execSync } from "node:child_process";
import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// Ayarlar'daki sürüm, indirme sayfasındaki adla aynı: "0.2.0 · 98365ec".
function gitSha(): string {
  try {
    return execSync("git rev-parse --short=7 HEAD", { stdio: ["ignore", "pipe", "ignore"] }).toString().trim();
  } catch {
    return "";
  }
}
const sha = gitSha();
const version = (process.env.npm_package_version ?? "0.0.0") + (sha ? ` · ${sha}` : "");

export default defineConfig({
  plugins: [react()],
  define: { __APP_VERSION__: JSON.stringify(version) },
  build: { outDir: "dist", target: "es2022", sourcemap: true, chunkSizeWarningLimit: 1600 },
});
