import { defineConfig } from "vite";
import { fileURLToPath, URL } from "node:url";
import { readFileSync } from "node:fs";
import devtoolsJson from "vite-plugin-devtools-json";
import tailwindcss from "@tailwindcss/vite";
import { svelte } from "@sveltejs/vite-plugin-svelte";
import { router } from "sv-router/vite-plugin";
import compression from "vite-plugin-compression2";
import { execSync } from "node:child_process";

export default defineConfig(({ mode }) => {
  const isDev = mode === "development";
  // Shown in the Web UI next to the firmware version, so a page can always be tied
  // back to the application version and the exact commit it was built from.
  const { version: appVersion } = JSON.parse(
    readFileSync(new URL("./package.json", import.meta.url), "utf8")
  );
  const commit: string = execSync('git rev-parse --short=16 HEAD').toString().trim();
  const version: string = `${appVersion}+${commit}`;

  return {
    plugins: [
      svelte(),
      router({
        path: "src/routes",
        allLazy: false,
        js: false,
      }),
      tailwindcss(),
      ...(isDev ? [devtoolsJson()] : []),
      compression({
        // Brotli only: the assets have outgrown the littlefs partition, and shipping
        // both encodings would need roughly twice the space that exists. The firmware
        // serves either encoding, so an image built here works with this firmware
        // version and the one before it - see main/WebServerManager.cpp::resolveAsset.
        algorithms: [
          'brotliCompress'
        ],
        deleteOriginalAssets: true
      })
    ],
    define: { __DEV__: isDev, __VERSION__: JSON.stringify(version) },
    resolve: {
      alias: {
        "@": fileURLToPath(new URL("./src", import.meta.url)),
        "$lib": fileURLToPath(new URL("./src/lib", import.meta.url)),
      },
    },
    build: {
      cssMinify: 'lightningcss',
      minify: 'oxc',
      target: 'es2020',
      rolldownOptions: {
        output: {
          codeSplitting: {
            minSize: 10000,
            groups: [
              {
                name: 'vendor',
                test: /node_modules/,
                priority: 2,
              },
              {
                name: 'components',
                test: /src\/lib\/components/,
                priority: 1,
              }
            ],
          },
          // console/debugger statements have to be dropped here: the `esbuild.drop`
          // option below has no effect under this rolldown/oxc pipeline, and the
          // firmware ships the bundle inside a 128 kB littlefs partition.
          minify: {
            compress: { dropConsole: true, dropDebugger: true },
          },
          polyfillRequire: false,
        }
      }
    },
    esbuild: {
      legalComments: 'none',
    },
    server: {
      proxy: {
        "/certificates": "http://localhost:8000",
        "/config": "http://localhost:8000",
        "/eth_get_config": "http://localhost:8000",
        "/nfc_get_presets": "http://localhost:8000",
        "/ws": { target: "ws://localhost:8000", ws: true },
        "/captive_portal_config": "http://localhost:8000"
      },
    },
  };
});
