<script lang="ts">
	import { onMount } from 'svelte';

	let data = $state<any>(null);
	let error = $state('');
	let loading = $state(true);

	onMount(async () => {
		try {
			const r = await fetch('/node');
			if (!r.ok) throw new Error((await r.json()).error || `${r.status}`);
			data = await r.json();
		} catch (e: any) {
			error = e.message;
		} finally {
			loading = false;
		}
	});
</script>

<div class="p-6 max-w-3xl">
	<h1 class="text-2xl font-bold mb-4">Node</h1>

	{#if loading}
		<div class="skeleton h-40 w-full"></div>
	{:else if error}
		<div class="alert alert-error">{error}</div>
	{:else}
		<div class="card bg-base-200">
			<div class="card-body gap-1">
				<div class="flex justify-between py-1">
					<span class="font-semibold">Node ID</span><code>{data.node_id}</code>
				</div>
				<div class="flex justify-between py-1">
					<span class="font-semibold">Name</span><span>{data.node_name}</span>
				</div>
				<div class="flex justify-between py-1">
					<span class="font-semibold">Role</span><span>{data.node_role}</span>
				</div>
				<div class="flex justify-between py-1">
					<span class="font-semibold">State</span><span>{data.node_state}</span>
				</div>
				<div class="flex justify-between py-1">
					<span class="font-semibold">Generation</span><span>{data.generation}</span>
				</div>
				<div class="flex justify-between py-1">
					<span class="font-semibold">Household</span><code>{data.household_id || '—'}</code>
				</div>
				<div class="py-1">
					<span class="font-semibold block mb-1">Public key</span>
					<code class="text-xs break-all">{data.public_key}</code>
				</div>
				<div class="py-1">
					<span class="font-semibold block mb-1">Certificate fingerprint</span>
					<code class="text-xs break-all">{data.cert_fingerprint}</code>
				</div>
			</div>
		</div>
	{/if}
</div>
