// Copy to firebase-config.js for local use / deploy:
//   cp firebase-config.example.js firebase-config.js
//
// NOTE: Firebase *web* config (apiKey, appId, databaseURL) is visible in the
// browser after you host the site. That is normal. Protect data with
// Realtime Database rules — never put the Database Secret here.

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
