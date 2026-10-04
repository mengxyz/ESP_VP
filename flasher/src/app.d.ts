/// <reference types="vite/client" />

interface Navigator {
  serial?: Serial;
}

interface Serial {
  requestPort(options?: { filters?: Array<{ usbVendorId?: number; usbProductId?: number }> }): Promise<SerialPort>;
}

interface SerialPort extends EventTarget {
  getInfo(): { usbVendorId?: number; usbProductId?: number };
}
