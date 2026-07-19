import { useEffect, useRef, useState } from "react";

import { Game } from "./game/Game";
import type { GameStatus } from "./game/types";

const INITIAL_STATUS: GameStatus = {
  seed: "—",
  loaded: 0,
  expected: 25,
  position: { x: 0, y: 0, z: 0 },
  flying: false,
  flySpeed: 24,
  aimDistance: null,
  farCoverage: { installedKm: 0, requestedKm: 0, loading: false, failed: false },
  ready: false,
  message: "Connecting to terrain generator…",
};

function formatCoordinate(value: number): string {
  return `${Math.round(value).toLocaleString()} m`;
}

function formatDistance(value: number | null): string {
  if (value === null) return "—";
  if (value < 1_000) return `${value.toFixed(value < 100 ? 1 : 0)} m`;
  return `${(value / 1_000).toFixed(value < 10_000 ? 2 : 1)} km`;
}

export function App() {
  const viewportRef = useRef<HTMLDivElement>(null);
  const gameRef = useRef<Game | null>(null);
  const [status, setStatus] = useState<GameStatus>(INITIAL_STATUS);
  const [locked, setLocked] = useState(false);

  useEffect(() => {
    const viewport = viewportRef.current;
    if (!viewport) return;

    const game = new Game(viewport, setStatus, setLocked);
    gameRef.current = game;
    void game.start().catch((error: unknown) => {
      const message = error instanceof Error ? error.message : String(error);
      setStatus((current) => ({ ...current, message: `Unable to start: ${message}` }));
    });

    return () => {
      gameRef.current = null;
      game.dispose();
    };
  }, []);

  const progress = Math.min(1, status.loaded / Math.max(1, status.expected));
  const failed = status.message.startsWith("Unable to start");
  const teleporting = status.message.startsWith("Searching a distant");

  return (
    <main className="game-shell">
      <div ref={viewportRef} className="viewport" />

      <section className="hud" aria-label="Terrain explorer heads-up display">
        <div className="brand">
          <strong>Terrain Stream</strong>
          <span>Seed {status.seed}</span>
        </div>

        <button
          className="random-terrain"
          type="button"
          disabled={!status.ready}
          onClick={() => void gameRef.current?.teleportRandom()}
        >
          <span>{teleporting ? "Locating terrain…" : "Random terrain"}</span>
          <kbd>R</kbd>
        </button>

        <div className="coordinates">
          <span>Human scale · eye 1.68 m · 1 unit = 1 m</span>
          <span>
            X {formatCoordinate(status.position.x)} · Y {formatCoordinate(status.position.y)} · Z{" "}
            {formatCoordinate(status.position.z)}
          </span>
          <span>
            Far geometry {status.farCoverage.failed
              ? "failed"
              : status.farCoverage.loading
                ? `${status.farCoverage.installedKm || 0} → ${status.farCoverage.requestedKm} km loading`
                : `${status.farCoverage.installedKm} km`}
          </span>
        </div>

        <div className="controls">
          <span>WASD Move</span>
          <span>Space Jump</span>
          <span>Shift Run</span>
          <span>F {status.flying ? "Walk" : "Fly"}</span>
          <span>R Random terrain</span>
          {status.flying && <span>C Descend · Wheel {Math.round(status.flySpeed)} m/s</span>}
        </div>

        {locked && (
          <div className="rangefinder" aria-label={`Distance ${formatDistance(status.aimDistance)}`}>
            <div className="crosshair" aria-hidden="true" />
            <output>{formatDistance(status.aimDistance)}</output>
          </div>
        )}

        {!locked && (
          <button
            className="enter-world"
            type="button"
            disabled={!status.ready}
            onClick={() => status.ready && gameRef.current?.requestPointerLock()}
          >
            <strong>{status.ready ? "Enter world" : failed ? "World unavailable" : "Generating terrain"}</strong>
            <span>{status.message}</span>
          </button>
        )}

        <div className="stream-progress" aria-hidden="true">
          <div style={{ transform: `scaleX(${progress})` }} />
        </div>
      </section>
    </main>
  );
}
