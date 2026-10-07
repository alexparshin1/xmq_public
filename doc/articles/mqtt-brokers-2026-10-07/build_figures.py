"""Extract the dated reports and make publication figures. Requires matplotlib."""
from pathlib import Path
import json
import re
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[2] / 'LoadTest/results/AWS/2026-10-07'
ORDER = ['Mosquitto', 'EMQX', 'FlashMQ', 'HiveMQ', 'XMQ']
COLORS = dict(zip(ORDER, ['#6b7280', '#d97706', '#2563eb', '#9333ea', '#059669']))
DATA = {}
for scenario in ['Connections', 'Fan-In', 'Fan-Out', 'Point-To-Point', 'Persistence']:
    records = {}
    report = (SOURCE / (scenario + '.txt')).read_text()
    for block in re.split(r'\n\nServer:   ', report)[1:]:
        broker = block.splitlines()[0]
        version = re.search(r'^Version:\s+(.*)$', block, re.M).group(1)
        cpu = re.search(r'CPU Load:\s+(\d+)% mean / (\d+)% peak', block)
        ram = re.search(r'Max RAM:\s+([\d.]+) (\w+)', block)
        rows = [dict(time_ms=int(t), count=int(n), latency_us=int(lat), rate_s=int(rate), rss_reported_mb=int(rss))
                for t, n, lat, rate, rss in re.findall(r'^(\d+)ms\s+(\d+)\s+(\d+)us\s+(\d+)\s+(\d+) Mb$', block, re.M)]
        summary = re.search(r'^Average\s+(\d+)\s+(\d+)us\s+(\d+)(?:\s+\d+ Mb)?$', block, re.M)
        records[broker] = dict(version=version, cpu_mean_pct=int(cpu[1]), cpu_peak_pct=int(cpu[2]),
                               ram_peak_reported=float(ram[1]), ram_unit_reported=ram[2], intervals=rows)
        if summary:
            records[broker].update(count=int(summary[1]), average_latency_us=int(summary[2]), reported_rate_s=int(summary[3]))
            assert sum(r['count'] for r in rows) == int(summary[1])
        else:
            records[broker]['status'] = re.search(r'^Status:\s+(.*)$', block, re.M).group(1)
    assert set(records) == set(ORDER), (scenario, records.keys())
    DATA[scenario] = records
(HERE / 'results.json').write_text(json.dumps(DATA, indent=2) + '\n')

plt.rcParams.update({'font.family': 'DejaVu Sans', 'font.size': 10, 'axes.spines.top': False,
                     'axes.spines.right': False, 'savefig.facecolor': 'white'})

LABELS = {'Mosquitto': 'Mosquitto', 'EMQX': 'EMQX', 'FlashMQ': 'FlashMQ', 'HiveMQ': 'HiveMQ CE', 'XMQ': 'XMQ'}
# The same marker per broker as on xmq.sptk.net: XMQ and FlashMQ often coincide, a ring and a cross stay visible.
MARKERS = {'Mosquitto': 's', 'EMQX': '^', 'FlashMQ': 'x', 'HiveMQ': 'D', 'XMQ': 'o'}
SCENARIOS = [('Fan-In', 'Fan-in · цель 50 000/с'), ('Fan-Out', 'Fan-out · цель 250 000/с'),
             ('Point-To-Point', 'Point-to-point · цель 100 000/с'), ('Persistence', 'Persistence · цель 60 000/с')]


def latency_text(ms):
    """Russian style: decimal comma, unit chosen by size."""
    if ms >= 1000:
        value, unit = ms / 1000, 'с'
    elif ms >= 1:
        value, unit = ms, 'мс'
    else:
        value, unit = ms * 1000, 'мкс'
    digits = 0 if value >= 100 else 1 if value >= 10 else 2
    return f'{value:.{digits}f}'.replace('.', ',') + ' ' + unit


fig, axes = plt.subplots(1, 4, figsize=(18, 5), layout='constrained')
for ax, (scenario, title) in zip(axes, SCENARIOS):
    values = [DATA[scenario][b]['average_latency_us'] / 1000 for b in ORDER]
    ax.barh([LABELS[b] for b in ORDER], values, color=[COLORS[b] for b in ORDER])
    ax.set_xscale('log')
    ax.set_xlim(.1, 1_000_000)
    ax.invert_yaxis()
    ax.set_title(title)
    ax.set_xlabel('Средняя задержка, мс (лог. шкала)')
    ax.grid(axis='x', alpha=.15)
    for i, value in enumerate(values):
        ax.text(value * 1.15, i, latency_text(value), va='center', fontsize=9)
fig.suptitle('AWS, 7 октября 2026 · MQTT 5 · QoS 1 · 16 байт', fontsize=14)
fig.savefig(HERE / 'latency-overview.png', dpi=160, bbox_inches='tight')
plt.close(fig)

# Every broker on one axis per scenario, as the site draws it: a log scale shows microseconds and
# minutes together, so no panel needs a scale of its own that makes a flat line look like a jump.
fig, axes = plt.subplots(2, 2, figsize=(13, 8.5), layout='constrained')
for ax, (scenario, title) in zip(axes.flat, SCENARIOS):
    for b in ORDER:
        rows = DATA[scenario][b]['intervals']
        ax.plot([r['time_ms'] / 1000 for r in rows], [r['latency_us'] / 1000 for r in rows],
                marker=MARKERS[b], markersize=7, markerfacecolor='none' if b == 'XMQ' else COLORS[b],
                color=COLORS[b], label=LABELS[b])
    ax.set_yscale('log')
    ax.set_ylim(.1, 1_000_000)
    ax.set_title(title)
    ax.set_xlabel('Начало интервала, с')
    ax.set_ylabel('Средняя задержка в интервале, мс')
    ax.grid(alpha=.2)
handles, labels = axes.flat[0].get_legend_handles_labels()
fig.legend(handles, labels, loc='outside lower center', ncol=5, frameon=False)
fig.suptitle('Задержка по интервалам · логарифмическая шкала', fontsize=14)
fig.savefig(HERE / 'latency-timeline.png', dpi=160)
plt.close(fig)
print('Verified 25 broker/scenario records; wrote results.json and two figures.')
