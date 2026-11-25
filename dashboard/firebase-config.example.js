// This file is only a reference.
// Real config is generated from .env (local) or Vercel Environment Variables:
//   npm run build
// which creates firebase-config.js (gitignored).
//
// NOTE: After hosting, these values are still visible in the browser.
// .env only keeps them out of GitHub.

export const firebaseConfig = {
  apiKey: "YOUR_API_KEY",
  authDomain: "YOUR-PROJECT.firebaseapp.com",
  databaseURL: "https://YOUR-PROJECT-default-rtdb.asia-southeast1.firebasedatabase.app",
  projectId: "YOUR-PROJECT",
  storageBucket: "YOUR-PROJECT.appspot.com",
  messagingSenderId: "",
  appId: "",
  measurementId: "",
};

export const DEVICE_ID = "pier-01";
export const HISTORY_POINTS = 360;
export const OFFLINE_AFTER_MS = 15000;
