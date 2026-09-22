<script lang="ts">
	import { onMount } from 'svelte';

	let data = $state<any>(null);
	let error = $state('');
	let loading = $state(true);

	onMount(async () => {
		try {
			const r = await fetch('/security');
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
	<h1 class="text-2xl font-bold mb-4">Security</h1>

	{#if loading}
		<div class="skeleton h-40 w-full"></div>
	{:else if error}
		<div class="alert alert-error">{error}</div>
	{:else}
		<div class="card bg-base-200">
			<div class="card-body gap-2">
				{#each data.findings ?? [] as f}
					<div class="flex justify-between items-center py-1 border-b border-base-300 last:border-0">
						<div>
							<div class="font-semibold">{f.component}</div>
							<div class="text-xs opacity-70">{f.detail}</div>
						</div>
						<span
							class:badge-success={f.status === 'OK'}
							class:badge-warning={f.status === 'WARNING'}
							class:badge-error={f.status === 'DISABLED'}
							class="badge">{f.status}</span
						>
					</div>
				{/each}
			</div>
		</div>
	{/if}
</div>
