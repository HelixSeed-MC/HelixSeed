/// <reference types="vite/client" />

import type { HelixSeedApi } from "../preload/preload";

declare global {
  interface Window {
    helixSeed: HelixSeedApi;
  }
}

export {};
