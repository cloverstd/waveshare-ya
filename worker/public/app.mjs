const $ = id => document.getElementById(id);
try {
  const response = await fetch('/api/v1/ota/manifest.json', {cache:'no-store'});
  if (!response.ok) throw new Error(response.status === 404 ? '尚无正式发布' : '更新服务暂不可用');
  const m = await response.json();
  const path = `/firmware/waveshare-ya/${m.version}/waveshare-ya-${m.version}.bin`;
  if (!/^\d+\.\d+\.\d+$/.test(m.version) || new URL(m.url).pathname !== path || !/^[a-f0-9]{64}$/.test(m.sha256)) throw new Error('发布信息无效');
  $('version').textContent = `v${m.version}`;
  $('notes').textContent = m.notes || '查看发布记录了解本次更新。';
  $('size').textContent = `${(m.size/1024/1024).toFixed(2)} MiB · RSA-PSS 签名应用`;
  $('download').href = path; $('download').hidden = false;
  $('release').href = `https://github.com/cloverstd/waveshare-ya/releases/tag/v${m.version}`;
  $('sha').textContent = m.sha256; $('checksum').hidden = false;
} catch (error) {
  $('version').textContent = error.message;
  $('notes').textContent = '可以稍后刷新，或前往 GitHub 查看发布记录。';
}
