let config = { serverUrl: 'http://localhost:3000', apiKey: '', refreshInterval: 5 };
let allFiles = [];
let engineData = {};

document.addEventListener('DOMContentLoaded', async () => {
  config = await window.electronAPI.getConfig();
  loadSettings();
  await refreshAll();

  // Auto-scan all drives if catalog is empty (Overview shows 0 files)
  try {
    const tiers = await apiGet('/api/v1/tiers');
    if (tiers && tiers.tiers) {
      const totalFiles = tiers.tiers.reduce((s, t) => s + (t.file_count || 0), 0);
      if (totalFiles === 0) {
        showToast('Catalog empty - scanning all drives...', 'info');
        const drivesData = await apiGet('/api/v1/drives');
        if (drivesData && drivesData.drives && drivesData.drives.length > 0) {
          let totalAdded = 0;
          for (const drive of drivesData.drives) {
            const mount = drive.mount_point;
            if (!mount) continue;
            showToast(`Scanning ${drive.label || mount}...`, 'info');
            try {
              // 60 second timeout per drive
              const controller = new AbortController();
              const timer = setTimeout(() => controller.abort(), 60000);
              const resp = await fetch(`${config.serverUrl}/api/v1/scan`, {
                method: 'POST',
                headers: headers(),
                body: JSON.stringify({ path: mount }),
                signal: controller.signal
              });
              clearTimeout(timer);
              if (resp.ok) {
                const result = await resp.json();
                if (result && result.added) totalAdded += result.added;
              }
            } catch (e) { /* skip failed/timed-out drives */ }
          }
          if (totalAdded > 0) {
            showToast(`Scan complete: ${totalAdded} files added`, 'success');
            await refreshAll();
          }
        }
      }
    }
  } catch (e) { /* ignore scan errors */ }

  window.electronAPI.onEngineState((data) => {
    updateStatusBadge(data.state || 'unknown');
    engineData = data;
  });

  window.electronAPI.onConfigChanged((cfg) => {
    config = cfg;
    loadSettings();
  });

  window.electronAPI.onRunCycle(() => runCycle());

  // Tab switching
  document.querySelectorAll('.nav-btn').forEach(btn => {
    btn.addEventListener('click', () => {
      document.querySelectorAll('.nav-btn').forEach(b => b.classList.remove('active'));
      document.querySelectorAll('.tab-content').forEach(t => t.classList.remove('active'));
      btn.classList.add('active');
      document.getElementById('tab-' + btn.dataset.tab).classList.add('active');
    });
  });

  // Auto-refresh
  setInterval(refreshAll, (config.refreshInterval || 5) * 1000);
});

function headers() {
  const h = { 'Content-Type': 'application/json' };
  const key = config.apiKey || '';
  if (key) h['X-API-Key'] = key;
  return h;
}

async function apiGet(path) {
  try {
    const resp = await fetch(`${config.serverUrl}${path}`, { headers: headers() });
    if (!resp.ok) {
      if (resp.status === 401) throw new Error('Unauthorized - check API Key in Settings');
      throw new Error(`HTTP ${resp.status}`);
    }
    return await resp.json();
  } catch (e) {
    showToast(`API error: ${e.message}`, 'error');
    throw e;
  }
}

async function apiPost(path, body) {
  try {
    const resp = await fetch(`${config.serverUrl}${path}`, {
      method: 'POST', headers: headers(), body: body ? JSON.stringify(body) : undefined,
    });
    if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
    return await resp.json();
  } catch (e) {
    showToast(`API error: ${e.message}`, 'error');
    throw e;
  }
}

// ── Overview ──

async function refreshAll() {
  try {
    const [tiers, savings, history, engine, drives] = await Promise.allSettled([
      apiGet('/api/v1/tiers'),
      apiGet('/api/v1/savings'),
      apiGet('/api/v1/cycle/history?n=10'),
      apiGet('/api/v1/engine'),
      apiGet('/api/v1/drives'),
    ]);
    if (tiers.status === 'fulfilled') renderTiers(tiers.value.tiers || tiers.value);
    if (savings.status === 'fulfilled') renderSavings(savings.value);
    if (engine.status === 'fulfilled') renderStatus(engine.value);
    if (history.status === 'fulfilled') renderRecentMigrations(history.value.migrations || []);
    if (drives.status === 'fulfilled') renderDrives(drives.value.drives || []);
    renderFiles();
  } catch (e) {
    // Already handled by apiGet
  }
}

function renderTiers(tiers) {
  const names = ['hot', 'warm', 'cold', 'archive'];
  const cols = ['#ef4444', '#f59e0b', '#3b82f6', '#8b5cf6'];
  const grid = document.getElementById('tier-cards');
  if (!tiers || tiers.length === 0) {
    grid.innerHTML = '<p class="loading">No tier data</p>';
    return;
  }
  grid.innerHTML = tiers.map((t, i) => {
    const cls = names[i] || 'hot';
    const gb = (t.total_bytes / 1e9).toFixed(1);
    return `
      <div class="tier-card ${cls}">
        <div class="tier-name" style="color:${cols[i]}">${t.tier}</div>
        <div class="tier-count">${t.file_count}</div>
        <div class="tier-sub">${gb} GB · ${t.accesses || 0} accesses</div>
        <div class="tier-cost">$${(t.monthly_cost || 0).toFixed(2)}/mo</div>
      </div>`;
  }).join('');
}

function renderSavings(data) {
  document.getElementById('cost-content').innerHTML = `
    <div style="display:grid;grid-template-columns:1fr 1fr;gap:8px;font-size:14px;">
      <div><span class="dim">Monthly Cost</span><br><strong>$${(data.current_monthly_cost || 0).toFixed(2)}</strong></div>
      <div><span class="dim">vs All-HOT</span><br><strong style="color:var(--success)">-$${(data.savings_vs_all_hot || 0).toFixed(2)}</strong></div>
      <div><span class="dim">Savings %</span><br><strong style="color:var(--success)">${(data.savings_percent || 0).toFixed(1)}%</strong></div>
      <div><span class="dim">Annual Projected</span><br><strong>$${(data.projected_annual_savings || 0).toFixed(2)}</strong></div>
    </div>`;
}

function renderStatus(data) {
  if (!data) return;
  engineData = data;
  updateStatusBadge(data.state || 'unknown');
  const state = data.state || 'unknown';
  document.getElementById('status-content').innerHTML = `
    <div style="display:grid;grid-template-columns:1fr 1fr;gap:8px;font-size:14px;">
      <div><span class="dim">Engine State</span><br><strong style="text-transform:capitalize">${state}</strong></div>
      <div><span class="dim">Files Managed</span><br><strong>${data.file_count || '—'}</strong></div>
      <div><span class="dim">Total Size</span><br><strong>${formatBytes(data.total_bytes || 0)}</strong></div>
      <div><span class="dim">Cycles Run</span><br><strong>${data.cycle_count || 0}</strong></div>
      <div><span class="dim">Total Migrations</span><br><strong>${data.total_migrations || 0}</strong></div>
      <div><span class="dim">Bytes Migrated</span><br><strong>${formatBytes(data.total_bytes_migrated || 0)}</strong></div>
    </div>`;
}

function updateStatusBadge(state) {
  const badge = document.getElementById('status-badge');
  badge.className = 'status-badge status-' + state;
  badge.textContent = '● ' + state.charAt(0).toUpperCase() + state.slice(1);
}

function renderRecentMigrations(migrations) {
  const container = document.getElementById('recent-migrations');
  if (!migrations || migrations.length === 0) {
    container.innerHTML = '<p class="dim">No migrations yet</p>';
    return;
  }
  container.innerHTML = `
    <table>
      <thead><tr>
        <th>Time</th><th>File</th><th>From</th><th>To</th><th>Size</th><th>Status</th>
      </tr></thead>
      <tbody>${migrations.slice(0, 10).map(m => {
        const fromTier = (m.from_tier ?? m.source_tier ?? '?').toLowerCase();
        const toTier = (m.to_tier ?? m.target_tier ?? '?').toLowerCase();
        return `<tr>
          <td>${m.timestamp || m.migrated_at || '—'}</td>
          <td style="max-width:200px;overflow:hidden;text-overflow:ellipsis">${m.file_path || m.file_name || '—'}</td>
          <td><span class="tier-badge ${fromTier}">${fromTier}</span></td>
          <td><span class="tier-badge ${toTier}">${toTier}</span></td>
          <td>${formatBytes(m.size_bytes || 0)}</td>
          <td>${m.success ? '✅' : '❌'}</td>
        </tr>`;
      }).join('')}</tbody>
    </table>`;
}

// ── Files ──

async function renderFiles() {
  try {
    const data = await apiGet('/api/v1/files');
    allFiles = data.files || [];
    filterFiles();
  } catch (e) { /* handled */ }
}

function filterFiles() {
  const search = (document.getElementById('file-search').value || '').toLowerCase();
  const tierFilter = document.getElementById('tier-filter').value;

  let filtered = allFiles;
  if (search) filtered = filtered.filter(f =>
    (f.path || f.name || f.id || '').toLowerCase().includes(search)
  );
  if (tierFilter !== '') filtered = filtered.filter(f => f.current_tier == tierFilter);

  const tbody = document.getElementById('files-body');
  if (filtered.length === 0) {
    tbody.innerHTML = '<tr><td colspan="9" class="loading">No files found</td></tr>';
    return;
  }

  tbody.innerHTML = filtered.map(f => {
    const tier = (f.current_tier !== undefined ? f.current_tier : f.tier);
    const tierNum = typeof tier === 'number' ? tier : parseInt(tier);
    const tierName = ['HOT','WARM','COLD','ARCHIVE'][tierNum] || '?';
    const tierLower = tierName.toLowerCase();
    const isPinned = f.is_pinned || false;
    const fileName = f.path ? f.path.split(/[\/\\]/).pop() : (f.name || f.id || '?');

    return `<tr>
      <td style="max-width:200px;overflow:hidden;text-overflow:ellipsis" title="${f.path || ''}">${fileName}</td>
      <td><span class="tier-badge ${tierLower}">${tierName}</span></td>
      <td>${formatBytes(f.size_bytes || 0)}</td>
      <td>${f.score !== undefined ? f.score.toFixed(1) : '—'}</td>
      <td>${f.access_count || 0}</td>
      <td>${f.idle_days !== undefined ? f.idle_days.toFixed(1) + 'd' : '—'}</td>
      <td>${f.file_type || '—'}</td>
      <td>$${(f.monthly_cost || 0).toFixed(2)}</td>
      <td class="action-cell">
        <button class="pin-btn ${isPinned ? 'pinned' : ''}" onclick="togglePin('${f.id}', ${isPinned})">
          ${isPinned ? 'Unpin' : 'Pin'}
        </button>
        ${tierNum >= 2 ? `<button class="recall-btn" onclick="recallFile('${f.id}')">Recall</button>` : ''}
      </td>
    </tr>`;
  }).join('');
}

async function togglePin(fileId, currentlyPinned) {
  try {
    await apiPost(`/api/v1/files/${fileId}/pin`, { pin: !currentlyPinned });
    showToast(`File ${currentlyPinned ? 'unpinned' : 'pinned'}`, 'success');
    renderFiles();
  } catch (e) { /* handled */ }
}

async function recallFile(fileId) {
  try {
    const result = await apiPost(`/api/v1/files/${fileId}/pin`, { pin: false });
    showToast(`File ${fileId} unpinned and will be re-evaluated`, 'info');
    if (result) await refreshAll();
  } catch (e) { /* handled */ }
}

// ── History ──

async function renderHistory() {
  try {
    const data = await apiGet('/api/v1/cycle/history?n=50');
    const migrations = data.migrations || [];
    const tbody = document.getElementById('history-body');

    if (migrations.length === 0) {
      tbody.innerHTML = '<tr><td colspan="8" class="loading">No migration history</td></tr>';
      return;
    }

    tbody.innerHTML = migrations.map(m => {
      const fromTier = (m.from_tier ?? m.source_tier ?? '').toLowerCase();
      const toTier = (m.to_tier ?? m.target_tier ?? '').toLowerCase();
      return `<tr>
        <td>${m.timestamp || m.migrated_at || '—'}</td>
        <td style="max-width:200px;overflow:hidden;text-overflow:ellipsis" title="${m.file_path || ''}">${m.file_path || m.file_name || '—'}</td>
        <td>${fromTier ? `<span class="tier-badge ${fromTier}">${fromTier}</span>` : '—'}</td>
        <td>${toTier ? `<span class="tier-badge ${toTier}">${toTier}</span>` : '—'}</td>
        <td>${formatBytes(m.size_bytes || 0)}</td>
        <td>${m.reason || '—'}</td>
        <td>${m.duration_ms ? (m.duration_ms / 1000).toFixed(1) + 's' : '—'}</td>
        <td>${m.success ? '✅' : '❌'}</td>
      </tr>`;
    }).join('');
  } catch (e) { /* handled */ }
}

// ── Drives ──

function renderDrives(drives) {
  const grid = document.getElementById('drives-grid');
  if (!drives || drives.length === 0) {
    grid.innerHTML = '<p class="dim">No drive data available</p>';
    return;
  }
  grid.innerHTML = drives.map(d => {
    const pct = d.usage_pct || 0;
    const tierName = (d.storage_class || d.tier || '?').toLowerCase();
    const colors = { hot: '#ef4444', warm: '#f59e0b', cold: '#3b82f6', archive: '#8b5cf6' };
    return `
      <div class="drive-card">
        <div class="drive-label">${d.label || d.mount_point || 'Unknown Drive'}</div>
        <div class="drive-type">
          ${d.hardware_name || 'Drive'} · <span class="tier-badge ${tierName}">${d.storage_class || d.tier || '?'}</span>
        </div>
        <div class="drive-bar">
          <div class="drive-bar-fill" style="width:${pct}%;background:${colors[tierName] || colors.hot}"></div>
        </div>
        <div class="drive-stats">
          <span>${formatBytes(d.used_bytes || 0)} / ${formatBytes(d.total_bytes || 0)}</span>
          <span>${pct.toFixed(1)}%</span>
        </div>
        <div style="font-size:11px;color:var(--text-dim);margin-top:4px">
          Free: ${formatBytes(d.free_bytes || 0)}
        </div>
      </div>`;
  }).join('');
}

// ── Cycle ──

async function runCycle() {
  const btn = document.getElementById('btn-cycle');
  btn.disabled = true;
  btn.textContent = '⏳ Running...';
  try {
    const result = await apiPost('/api/v1/cycle');
    showToast(`Cycle complete: ${result.migrated || 0} files migrated, $${(result.monthly_savings || 0).toFixed(2)}/mo saved`, 'success');
    await refreshAll();
  } catch (e) {
    showToast('Cycle failed', 'error');
  }
  btn.disabled = false;
  btn.textContent = '▶ Run Cycle';
}

// ── Scan ──

async function runScan() {
  const btn = document.getElementById('btn-scan');
  if (btn) { btn.disabled = true; btn.textContent = '⏳ Scanning...'; }
  try {
    const drivesData = await apiGet('/api/v1/drives');
    if (drivesData && drivesData.drives && drivesData.drives.length > 0) {
      let totalAdded = 0, totalScanned = 0, totalSkipped = 0;
      for (const drive of drivesData.drives) {
        const mount = drive.mount_point;
        if (!mount) continue;
        try {
          const result = await apiPost('/api/v1/scan', { path: mount });
          if (result) {
            totalAdded += result.added || 0;
            totalScanned += result.scanned || 0;
            totalSkipped += result.skipped || 0;
          }
        } catch (e) { /* skip failed drives */ }
      }
      showToast(`Scan complete: ${totalAdded} files added (${totalScanned} scanned, ${totalSkipped} skipped)`, 'success');
    } else {
      // Fallback: scan default path
      const result = await apiPost('/api/v1/scan');
      showToast(`Scan complete: ${result.added || 0} files added`, 'success');
    }
    await refreshAll();
  } catch (e) {
    showToast('Scan failed', 'error');
  }
  if (btn) { btn.disabled = false; btn.textContent = '🔍 Scan Files'; }
}

// ── Settings ──

function loadSettings() {
  document.getElementById('server-url').value = config.serverUrl || 'http://localhost:3000';
  document.getElementById('api-key').value = config.apiKey || '';
  document.getElementById('refresh-interval').value = config.refreshInterval || 5;
  document.getElementById('about-server').textContent = config.serverUrl || 'not configured';
}

async function saveSettings() {
  const newConfig = {
    serverUrl: document.getElementById('server-url').value.trim(),
    apiKey: document.getElementById('api-key').value.trim(),
    refreshInterval: parseInt(document.getElementById('refresh-interval').value) || 5,
  };
  config = await window.electronAPI.saveConfig(newConfig);
  const status = document.getElementById('settings-status');
  status.textContent = '✅ Saved';
  setTimeout(() => status.textContent = '', 2000);
  document.getElementById('about-server').textContent = config.serverUrl;
  showToast('Settings saved', 'success');
}

// ── Tab visibility ──

document.querySelector('[data-tab="history"]').addEventListener('click', () => {
  setTimeout(renderHistory, 100);
});

document.querySelector('[data-tab="drives"]').addEventListener('click', async () => {
  try {
    const data = await apiGet('/api/v1/drives');
    renderDrives(data.drives || []);
  } catch (e) { /* handled */ }
});

// ── Utilities ──

function formatBytes(bytes) {
  if (!bytes || bytes === 0) return '0 B';
  const units = ['B', 'KB', 'MB', 'GB', 'TB'];
  const i = Math.floor(Math.log(bytes) / Math.log(1024));
  return (bytes / Math.pow(1024, i)).toFixed(i > 0 ? 1 : 0) + ' ' + units[i];
}

function showToast(message, type = 'info') {
  const container = document.getElementById('toast-container');
  const toast = document.createElement('div');
  toast.className = `toast ${type}`;
  toast.textContent = message;
  container.appendChild(toast);
  setTimeout(() => {
    toast.style.opacity = '0';
    toast.style.transition = 'opacity 0.3s';
    setTimeout(() => toast.remove(), 300);
  }, 3500);
}
