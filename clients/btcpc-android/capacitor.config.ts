import type { CapacitorConfig } from '@capacitor/cli';

const config: CapacitorConfig = {
  appId: 'net.btcpc.app',
  appName: 'BTCPC',
  webDir: 'www',
  plugins: {
    StatusBar: {
      backgroundColor: '#0a0e17',
      style: 'DARK',
    },
  },
  android: {
    buildOptions: {
      keystorePath: undefined,
      keystoreAlias: undefined,
    },
  },
};

export default config;
