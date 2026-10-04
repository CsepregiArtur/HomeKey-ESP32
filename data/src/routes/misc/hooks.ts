import type { Hooks } from 'sv-router';
import type { ApiResponse, MiscConfig, NfcGpioPinsPreset } from '$lib/types/api';
import { setLoadingState } from '$lib/stores/system.svelte';

async function readJsonResponse<T>(response: Response, endpoint: string): Promise<T> {
  if (!response.ok) {
    throw new Error(`${endpoint} returned HTTP ${response.status} ${response.statusText}`);
  }

  try {
    return await response.json();
  } catch {
    throw new Error(`${endpoint} returned an invalid JSON response`);
  }
}

declare module 'sv-router' {
  interface RouteMeta {
    miscData?: { misc: MiscConfig | null; nfcPresets: NfcGpioPinsPreset | null; error: string | null };
  }
}

export default {
  async beforeLoad({ meta }) {
    try {
      setLoadingState(true);
      // Ethernet was removed from the firmware (the driver, its configuration and the
      // /eth_get_config endpoint), so this loader must not request it. Unknown paths are
      // answered by the SPA catch-all with index.html, which makes .json() throw
      // "The string did not match the expected pattern" and breaks the whole page.
      const [miscRes, nfcRes] = await Promise.all([
        fetch('/config?type=misc').then(r => readJsonResponse<ApiResponse<MiscConfig>>(r, '/config?type=misc')),
        fetch('/nfc_get_presets').then(r => readJsonResponse<ApiResponse<NfcGpioPinsPreset>>(r, '/nfc_get_presets')),
      ]);
      if (!miscRes.success) throw new Error(miscRes.error);
      if (!nfcRes.success) throw new Error(nfcRes.error);
      meta.miscData = {
        misc: miscRes.data as MiscConfig,
        nfcPresets: nfcRes.data as NfcGpioPinsPreset,
        error: null,
      };
    } catch (error) {
      console.error('Failed to load misc config:', error);
      meta.miscData = { misc: null, nfcPresets: null, error: error instanceof Error ? error.message : 'Unknown error' };
    } finally {
      setLoadingState(false);
    }
  },
} satisfies Hooks;
