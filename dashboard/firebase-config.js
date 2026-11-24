// Firebase web app config for project "Kaavya"
export const firebaseConfig = {
  apiKey: "AIzaSyAIBzRpdAGGg9mDGrrbcHp68r-Zj7HoT5w",
  authDomain: "kaavya-fb9e5.firebaseapp.com",
  databaseURL: "https://kaavya-fb9e5-default-rtdb.asia-southeast1.firebasedatabase.app",
  projectId: "kaavya-fb9e5",
  storageBucket: "kaavya-fb9e5.firebasestorage.app",
  messagingSenderId: "883118917225",
  appId: "1:883118917225:web:6aed319b0677ccdc8444f8",
  measurementId: "G-1BL48P5SE8",
};

// Must match DEVICE_ID in ScourSentry.ino
export const DEVICE_ID = "pier-01";

// History points shown in the charts (device uploads one every 10 s -> 360 = 1 hour)
export const HISTORY_POINTS = 360;

// Device is shown OFFLINE if no live update arrives within this time
export const OFFLINE_AFTER_MS = 15000;
