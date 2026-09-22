<script lang="ts">
	import { onMount } from 'svelte';

	let data = $state<any>(null);
	let error = $state('');
	let loading = $state(true);

	async function load() {
		loading = true;
		try {
			const r = await fetch('/audit');
			if (!r.ok) throw new Error((await r.json()).error || `${r.status}`);
			data = await r.json();
		} catch (e: any) {
			error = e.message;
		} finally {
			loading = false;
		}
	}

	onMount(load);
</script>

<div class="p-6 max-w-3xl">
	<h1 class="text-2xl font-bold mb-4">Audit</h1>

	{#if loading}
		<div class="skeleton h-40 w-full"></div>
	{:else if error}
		<div class="alert alert-error">{error}</div>
	{:else}
		<p class="text-sm opacity-70 mb-2">{data.total} records</p>
		<div class="overflow-x-auto">
			<table class="table table-zebra table-sm">
				<thead>
					<tr><th>Time</th><th>Event</th><th>Source</th><th>Result</th><th>Node</th><th>Meta</th></tr>
				</thead>
				<tbody>
					{#each data.records ?? [] as r}
						<tr>
							<td class="text-xs">{new Date(r.timestamp * 1000).toLocaleString()}</td>
							<td>{r.event}</td>
							<td>{r.source}</td>
							<td>{r.result}</td>
							<td class="text-xs">{r.node_id}</td>
							<td class="text-xs">{r.metadata}</td>
						</tr>
					{/each}
				</tbody>
			</table>
		</div>
	{/if}
</div>
