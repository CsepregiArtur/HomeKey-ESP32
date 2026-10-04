<script lang="ts">
	import AppMisc from '$lib/components/AppMisc.svelte';
	import { route } from 'sv-router/generated';
	import { systemInfo } from '$lib/stores/system.svelte.js';
</script>

<AppMisc misc={route.meta.miscData?.misc} error={route.meta.miscData?.error} nfcPresets={route.meta.miscData?.nfcPresets} nfcConnected={systemInfo.nfc_connected} />
