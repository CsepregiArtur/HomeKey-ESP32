<script lang="ts">
	let error = $state('');
	let message = $state('');
	let code = $state('');
	let ttl = $state(0);
	let busy = $state(false);

	let householdId = $state('');
	let householdName = $state('');
	let nodeRole = $state('other');
	let nodeName = $state('');

	async function issue() {
		busy = true;
		error = '';
		code = '';
		try {
			const r = await fetch('/provision/issue', { method: 'POST' });
			const res = await r.json();
			if (!r.ok) throw new Error(res.error || `${r.status}`);
			code = res.code;
			ttl = res.ttl_seconds || 0;
		} catch (e: any) {
			error = e.message;
		} finally {
			busy = false;
		}
	}

	async function join() {
		busy = true;
		error = '';
		message = '';
		try {
			const r = await fetch('/provision/join', {
				method: 'POST',
				headers: { 'Content-Type': 'application/json' },
				body: JSON.stringify({
					code,
					household_id: householdId,
					household_name: householdName || 'Household',
					node_role: nodeRole,
					node_name: nodeName
				})
			});
			const res = await r.json();
			if (!r.ok) throw new Error(res.error || `${r.status}`);
			message = res.message;
		} catch (e: any) {
			error = e.message;
		} finally {
			busy = false;
		}
	}
</script>

<div class="p-6 max-w-3xl">
	<h1 class="text-2xl font-bold mb-4">Provision</h1>

	{#if error}
		<div class="alert alert-error">{error}</div>
	{/if}
	{#if message}
		<div class="alert alert-success">{message}</div>
	{/if}

	<div class="card bg-base-200 mt-4">
		<div class="card-body gap-2">
			<div class="form-control">
				<label class="label" for="prov-household-id"><span class="label-text">Household ID</span></label>
				<input id="prov-household-id" class="input input-bordered" bind:value={householdId} placeholder="HOUSE-7F42" />
			</div>
			<div class="form-control">
				<label class="label" for="prov-household-name"><span class="label-text">Household name</span></label>
				<input id="prov-household-name" class="input input-bordered" bind:value={householdName} placeholder="My Household" />
			</div>
			<div class="form-control">
				<label class="label" for="prov-node-role"><span class="label-text">Node role</span></label>
				<select id="prov-node-role" class="select select-bordered" bind:value={nodeRole}>
					<option value="gate">Gate</option>
					<option value="main_house">Main house</option>
					<option value="small_house">Small house</option>
					<option value="garage">Garage</option>
					<option value="workshop">Workshop</option>
					<option value="other">Other</option>
				</select>
			</div>
			<div class="form-control">
				<label class="label" for="prov-node-name"><span class="label-text">Node name</span></label>
				<input id="prov-node-name" class="input input-bordered" bind:value={nodeName} placeholder="Gate" />
			</div>

			<div class="flex gap-2 mt-2">
				<button class="btn btn-outline" disabled={busy} onclick={issue}>1. Issue one-time code</button>
				<button class="btn btn-primary" disabled={busy || !code} onclick={join}>2. Join household</button>
			</div>
			{#if code}
				<p class="text-sm opacity-70 mt-2">One-time code: <code>{code}</code></p>
				{#if ttl}
					<p class="text-xs opacity-60">Expires in {ttl} seconds.</p>
				{/if}
			{/if}
		</div>
	</div>
</div>
