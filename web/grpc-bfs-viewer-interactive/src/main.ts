import * as PIXI from 'pixi.js';
import {
  forceCenter,
  forceCollide,
  forceLink,
  forceManyBody,
  forceSimulation,
  type SimulationLinkDatum,
  type SimulationNodeDatum,
} from 'd3-force';

import { GrpcWebFetchTransport } from '@protobuf-ts/grpcweb-transport';
import { TraversalServiceClient } from './gen/bfs_stream.client';
import type { BfsLayer, BfsStreamRequest } from './gen/bfs_stream';

console.log('[main] interactive CPGDB gRPC BFS client loaded');

function mustElement<T extends HTMLElement>(selector: string): T {
  const element = document.querySelector<T>(selector);

  if (!element) {
    throw new Error(`Missing required DOM element: ${selector}`);
  }

  return element;
}

const els = {
  endpoint: mustElement<HTMLInputElement>('#endpoint'),
  graphId: mustElement<HTMLInputElement>('#graphId'),
  sourceVertexId: mustElement<HTMLInputElement>('#sourceVertexId'),
  sourceType: mustElement<HTMLInputElement>('#sourceType'),
  maxDepth: mustElement<HTMLInputElement>('#maxDepth'),
  run: mustElement<HTMLButtonElement>('#run'),
  stop: mustElement<HTMLButtonElement>('#stop'),
  resetView: mustElement<HTMLButtonElement>('#resetView'),
  status: mustElement<HTMLPreElement>('#status'),
  host: mustElement<HTMLElement>('#canvasHost'),
};

type UiNode = SimulationNodeDatum & {
  id: string;
  label: string;
  depth: number;
  radius: number;
  view: PIXI.Container;
};

type UiLink = SimulationLinkDatum<UiNode> & {
  key: string;
  source: UiNode;
  target: UiNode;
};

const graph = {
  nodes: new Map<string, UiNode>(),
  links: [] as UiLink[],
};

let activeAbortController: AbortController | null = null;

function setStatus(message: string): void {
  console.log('[status]', message);
  els.status.textContent = message;
}

function appendStatus(message: string): void {
  console.log('[status+]', message);
  els.status.textContent = `${els.status.textContent}\n${message}`;
}

function setRunning(running: boolean): void {
  els.run.disabled = running;
  els.stop.disabled = !running;
}

function grpcEndpoint(): string {
  return els.endpoint.value.trim().replace(/\/$/, '');
}

function shortLabel(id: string): string {
  if (!id) {
    return '?';
  }

  return id.length <= 8 ? id : id.slice(0, 8);
}

const pixiApp = new PIXI.Application();

await pixiApp.init({
  resizeTo: els.host,
  background: '#111318',
  antialias: true,
});

els.host.appendChild(pixiApp.canvas);

const viewport = new PIXI.Container();
const interactionLayer = new PIXI.Graphics();
const world = new PIXI.Container();
const edgeLayer = new PIXI.Graphics();
const nodeLayer = new PIXI.Container();

world.addChild(edgeLayer);
world.addChild(nodeLayer);
viewport.addChild(world);
pixiApp.stage.addChild(interactionLayer);
pixiApp.stage.addChild(viewport);

function resizeInteractionLayer(): void {
  interactionLayer.clear();

  // Fully transparent graphics are not always hit-test friendly, so use tiny alpha.
  interactionLayer
    .rect(0, 0, pixiApp.renderer.width, pixiApp.renderer.height)
    .fill({ color: 0x000000, alpha: 0.001 });

  interactionLayer.hitArea = new PIXI.Rectangle(
    0,
    0,
    pixiApp.renderer.width,
    pixiApp.renderer.height,
  );
}

resizeInteractionLayer();

const simulation = forceSimulation<UiNode>([])
  .force('charge', forceManyBody<UiNode>().strength(-650))
  .force('center', forceCenter(pixiApp.renderer.width / 2, pixiApp.renderer.height / 2))
  .force('collision', forceCollide<UiNode>().radius((node) => node.radius + 22))
  .force(
    'link',
    forceLink<UiNode, UiLink>([])
      .id((node) => node.id)
      .distance(120)
      .strength(0.35),
  )
  .alphaDecay(0.045)
  .on('tick', render);

window.addEventListener('resize', () => {
  resizeInteractionLayer();

  simulation.force(
    'center',
    forceCenter(pixiApp.renderer.width / 2, pixiApp.renderer.height / 2),
  );

  simulation.alpha(0.4).restart();
});

function colorForDepth(depth: number): number {
  const palette = [
    0x8ea2ff,
    0x58d68d,
    0xf4d03f,
    0xeb984e,
    0xaf7ac5,
    0x5dade2,
    0xec7063,
  ];

  return palette[Math.abs(depth) % palette.length];
}

function resetView(): void {
  viewport.scale.set(1);
  viewport.position.set(0, 0);
  appendStatus('View reset.');
}

function zoomAt(screenX: number, screenY: number, factor: number): void {
  const oldScale = viewport.scale.x;
  const newScale = Math.min(4, Math.max(0.08, oldScale * factor));

  const beforeWorldX = (screenX - viewport.position.x) / oldScale;
  const beforeWorldY = (screenY - viewport.position.y) / oldScale;

  viewport.scale.set(newScale);
  viewport.position.set(
    screenX - beforeWorldX * newScale,
    screenY - beforeWorldY * newScale,
  );
}

function setupViewportInteraction(): void {
  let isPanning = false;
  let lastX = 0;
  let lastY = 0;

  interactionLayer.eventMode = 'static';
  interactionLayer.cursor = 'grab';

  interactionLayer.on('pointerdown', (event) => {
    isPanning = true;
    lastX = event.global.x;
    lastY = event.global.y;
    interactionLayer.cursor = 'grabbing';
  });

  interactionLayer.on('pointermove', (event) => {
    if (!isPanning) {
      return;
    }

    const dx = event.global.x - lastX;
    const dy = event.global.y - lastY;

    viewport.position.x += dx;
    viewport.position.y += dy;

    lastX = event.global.x;
    lastY = event.global.y;
  });

  const stopPan = () => {
    isPanning = false;
    interactionLayer.cursor = 'grab';
  };

  interactionLayer.on('pointerup', stopPan);
  interactionLayer.on('pointerupoutside', stopPan);
  interactionLayer.on('pointercancel', stopPan);

  els.host.addEventListener(
    'wheel',
    (event) => {
      event.preventDefault();

      const rect = els.host.getBoundingClientRect();
      const x = event.clientX - rect.left;
      const y = event.clientY - rect.top;
      const factor = Math.exp(-event.deltaY * 0.0012);

      zoomAt(x, y, factor);
    },
    { passive: false },
  );

  els.host.addEventListener('dblclick', () => {
    resetView();
  });
}

setupViewportInteraction();

function clearGraph(): void {
  graph.nodes.clear();
  graph.links.length = 0;

  nodeLayer.removeChildren();
  edgeLayer.clear();

  simulation.nodes([]);
  const linkForce = simulation.force('link') as ReturnType<typeof forceLink<UiNode, UiLink>>;
  linkForce.links([]);

  simulation.alpha(0).stop();
}

let draggedNode: UiNode | null = null;

function makeNodeInteractive(node: UiNode): void {
  node.view.eventMode = 'static';
  node.view.cursor = 'grab';

  node.view.on('pointerdown', (event) => {
    event.stopPropagation();

    draggedNode = node;
    node.view.cursor = 'grabbing';

    node.fx = node.x ?? 0;
    node.fy = node.y ?? 0;

    simulation.alphaTarget(0.25).restart();
  });

  node.view.on('pointermove', (event) => {
    if (draggedNode !== node) {
      return;
    }

    const pos = event.getLocalPosition(world);

    node.fx = pos.x;
    node.fy = pos.y;
    node.x = pos.x;
    node.y = pos.y;

    render();
  });
}

function endNodeDrag(): void {
  if (!draggedNode) {
    return;
  }

  draggedNode.view.cursor = 'grab';

  // Let the force simulation continue naturally after dragging.
  // If you prefer pinned nodes, remove these two lines.
  draggedNode.fx = null;
  draggedNode.fy = null;

  draggedNode = null;
  simulation.alphaTarget(0);
}

pixiApp.stage.eventMode = 'static';
pixiApp.stage.on('pointerup', endNodeDrag);
pixiApp.stage.on('pointerupoutside', endNodeDrag);
pixiApp.stage.on('pointercancel', endNodeDrag);
window.addEventListener('pointerup', endNodeDrag);

function ensureNode(id: string, depth = 0, label = shortLabel(id)): UiNode | null {
  if (!id) {
    return null;
  }

  const existing = graph.nodes.get(id);

  if (existing) {
    if (depth < existing.depth) {
      existing.depth = depth;
    }

    return existing;
  }

  const node: UiNode = {
    id,
    label,
    depth,
    x: pixiApp.renderer.width / 2 + (Math.random() - 0.5) * 100,
    y: pixiApp.renderer.height / 2 + (Math.random() - 0.5) * 100,
    radius: depth === 0 ? 15 : 10,
    view: new PIXI.Container(),
  };

  const circle = new PIXI.Graphics();

  circle
    .circle(0, 0, node.radius)
    .fill(colorForDepth(depth))
    .stroke({
      width: 2,
      color: 0xffffff,
      alpha: depth === 0 ? 0.9 : 0.25,
    });

  const text = new PIXI.Text({
    text: label,
    style: {
      fill: '#dbe4ff',
      fontSize: 11,
      align: 'center',
    },
  });

  text.anchor.set(0.5, -1.0);

  node.view.addChild(circle);
  node.view.addChild(text);
  nodeLayer.addChild(node.view);

  makeNodeInteractive(node);

  graph.nodes.set(id, node);

  return node;
}

function addLink(sourceId: string, targetId: string): void {
  if (!sourceId || !targetId || sourceId === targetId) {
    return;
  }

  const key = `${sourceId}->${targetId}`;

  if (graph.links.some((link) => link.key === key)) {
    return;
  }

  const source = graph.nodes.get(sourceId) ?? ensureNode(sourceId, 0);
  const target = graph.nodes.get(targetId) ?? ensureNode(targetId, 0);

  if (!source || !target) {
    return;
  }

  graph.links.push({
    key,
    source,
    target,
  });
}

function applyLayer(layer: BfsLayer): void {
  console.log('[grpc] layer received:', layer);

  if (layer.done) {
    appendStatus('Done.');
    return;
  }

  const depth = layer.depth ?? 0;
  const vertices = layer.vertices ?? [];
  const edges = layer.treeEdges ?? [];

  for (const vertex of vertices) {
    const id = vertex.id;
    const distance = vertex.distance ?? depth;

    ensureNode(
      id,
      distance,
      `${shortLabel(id)}\nd=${distance}`,
    );
  }

  for (const edge of edges) {
    addLink(edge.sourceId, edge.targetId);
  }

  const nodeArray = Array.from(graph.nodes.values());

  simulation.nodes(nodeArray);

  const linkForce = simulation.force('link') as ReturnType<typeof forceLink<UiNode, UiLink>>;
  linkForce.links(graph.links);

  simulation.alpha(Math.max(0.25, 0.9 - depth * 0.08)).restart();

  setStatus(
    `Received layer ${depth}\n` +
    `New vertices: ${vertices.length}\n` +
    `Total vertices: ${graph.nodes.size}\n` +
    `Tree edges: ${graph.links.length}\n\n` +
    `Controls: wheel zoom, drag background pan, drag node move.`,
  );
}

function render(): void {
  edgeLayer.clear();

  for (const link of graph.links) {
    edgeLayer
      .moveTo(link.source.x ?? 0, link.source.y ?? 0)
      .lineTo(link.target.x ?? 0, link.target.y ?? 0)
      .stroke({
        width: 1.4,
        color: 0x7f8aa3,
        alpha: 0.38,
      });
  }

  for (const node of graph.nodes.values()) {
    node.view.position.set(node.x ?? 0, node.y ?? 0);
  }
}

function buildRequest(): BfsStreamRequest {
  const graphId = els.graphId.value.trim();
  const sourceVertexId = els.sourceVertexId.value.trim();
  const sourceType = els.sourceType.value.trim() || 'Faculty';
  const maxDepth = Number(els.maxDepth.value || 0);

  if (!grpcEndpoint()) {
    throw new Error('Missing gRPC-Web endpoint');
  }

  if (!graphId) {
    throw new Error('Missing graph ID');
  }

  if (!sourceVertexId) {
    throw new Error('Missing source UUID');
  }

  if (!Number.isFinite(maxDepth) || maxDepth < 0) {
    throw new Error('Max depth must be a non-negative number');
  }

  return {
    graphId,
    sourceVertexId,
    sourceType,
    maxDepth,
    returnPredecessors: true,
    allowNativePushdown: false,
    allowPartialAcceleration: true,
  };
}

function looksLikeEndOfStreamTransportDisconnect(err: unknown): boolean {
  const text = err instanceof Error
    ? `${err.name}: ${err.message}\n${err.stack ?? ''}`
    : String(err);

  return (
    text.includes('HTTP POST failed') ||
    text.includes('Could not connect to server') ||
    text.includes('Failed to fetch') ||
    text.includes('NetworkError')
  );
}

async function startBfs(): Promise<void> {
  console.log('[ui] startBfs called');

  if (activeAbortController) {
    activeAbortController.abort();
    activeAbortController = null;
  }

  let sawAtLeastOneLayer = false;

  try {
    clearGraph();
    setRunning(true);

    const request = buildRequest();

    console.log('[grpc] endpoint:', grpcEndpoint());
    console.log('[grpc] request:', request);

    const transport = new GrpcWebFetchTransport({
      baseUrl: grpcEndpoint(),

      // For server streaming through Envoy's grpc_web filter.
      format: 'text',
    });

    const client = new TraversalServiceClient(transport);
    const abortController = new AbortController();
    activeAbortController = abortController;

    setStatus('Sending StreamBfs request...');

    const call = client.streamBfs(request, {
      abort: abortController.signal,
    });

    try {
      for await (const layer of call.responses) {
        if (abortController.signal.aborted) {
          appendStatus('Cancelled.');
          return;
        }

        sawAtLeastOneLayer = true;
        applyLayer(layer);
      }

      const status = await call.status;
      appendStatus(`status=${status.code} ${status.detail ?? ''}`);
    } catch (streamErr) {
      if (abortController.signal.aborted) {
        appendStatus('Cancelled.');
        return;
      }

      if (sawAtLeastOneLayer && looksLikeEndOfStreamTransportDisconnect(streamErr)) {
        console.warn('[grpc] stream ended with transport disconnect after data:', streamErr);
        appendStatus(
          'Stream produced data, then the HTTP/gRPC-Web transport disconnected at the end.\n' +
          'The graph is kept on screen.\n' +
          'Backend/Envoy likely closed the stream without clean gRPC-Web trailers.',
        );
        return;
      }

      throw streamErr;
    }
  } catch (err) {
    const message = err instanceof Error ? err.stack ?? err.message : String(err);

    console.error('[ui] startBfs failed:', err);
    setStatus(`Frontend/gRPC error:\n${message}`);
  } finally {
    activeAbortController = null;
    setRunning(false);
  }
}

function stopBfs(): void {
  console.log('[ui] stop clicked');

  if (activeAbortController) {
    activeAbortController.abort();
    activeAbortController = null;
    appendStatus('Cancelling stream...');
  } else {
    appendStatus('No active stream.');
  }

  setRunning(false);
}

els.run.addEventListener('click', () => {
  void startBfs();
});

els.stop.addEventListener('click', stopBfs);
els.resetView.addEventListener('click', resetView);

setRunning(false);
setStatus(
  'Idle. Run `npm run gen`, paste a source UUID, then click Run streaming BFS.\n' +
  'Controls: wheel zoom, drag background pan, drag node move.',
);
