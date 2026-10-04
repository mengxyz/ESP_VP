import { svelte } from "@sveltejs/vite-plugin-svelte";
import { defineConfig } from "vitest/config";

export default defineConfig({
  // Relative assets work both at /ESP_VP/ and in local previews.
  base: "./",
  plugins: [svelte()],
  test: {
    include: ["src/**/*.test.ts"],
  },
});
