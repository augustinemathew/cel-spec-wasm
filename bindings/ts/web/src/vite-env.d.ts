/// <reference types="vite/client" />

// Monaco's `MonacoEnvironment` worker hook, narrowed to the single
// accessor the demo sets.  Monaco reads `self.MonacoEnvironment.getWorker`
// to resolve its editor web worker; declaring it here keeps the bootstrap
// in `main.ts` typed without an `any`.
interface MonacoEnvironment {
  getWorker(workerId: string, label: string): Worker;
}

// eslint-disable-next-line no-var
declare var MonacoEnvironment: MonacoEnvironment | undefined;
