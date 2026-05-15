import { defineConfig } from "vite";
import path from "node:path";

export default defineConfig({
  root: ".",
  base: "./",
  build: {
    outDir: "dist/renderer",
    emptyOutDir: true,
    sourcemap: false,
    assetsInlineLimit: 0,
    rollupOptions: {
      input: {
        main: path.resolve(__dirname, "index.html"),
        guide: path.resolve(__dirname, "guide.html"),
        docs: path.resolve(__dirname, "docs.html")
      }
    }
  }
});
