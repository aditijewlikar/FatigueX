const express = require('express');
const http = require('http');
const WebSocket = require('ws');
const { MongoClient } = require('mongodb');
const cors = require('cors');
const bodyParser = require('body-parser');
const path = require('path');

const app = express();
app.use(cors());

// Add error handling to body-parser to prevent crashes on aborted requests
app.use((req, res, next) => {
    bodyParser.json()(req, res, (err) => {
        if (err) {
            console.error('Body parser error:', err);
            return res.status(400).send({ error: 'Invalid JSON or aborted request' });
        }
        next();
    });
});

app.use(express.static(path.join(__dirname, '../frontend')));

const server = http.createServer(app);
const wss = new WebSocket.Server({ server });

// Initialize MongoDB connection
const uri = "mongodb://127.0.0.1:27017";
const client = new MongoClient(uri);

let db;
let telemetryCollection;

async function runDB() {
    try {
        await client.connect();
        db = client.db("fatiguex");
        telemetryCollection = db.collection("telemetry");
        console.log("Connected to MongoDB");
    } catch (err) {
        console.log("MongoDB connection failed, proceeding without database.", err.message);
    }
}
runDB();

// Log incoming requests
app.use((req, res, next) => {
    // Only log errors or specific endpoints to avoid spam
    next();
});

// API Endpoint to receive telemetry from ESP32
app.post('/api/telemetry', async (req, res) => {
    try {
        const data = req.body;
        data.timestamp = new Date();

        if (telemetryCollection) {
            await telemetryCollection.insertOne(data).catch(e => console.log('DB Insert Error', e));
        }

        // Broadcast to all connected WebSocket clients
        wss.clients.forEach(client => {
            if (client.readyState === WebSocket.OPEN) {
                client.send(JSON.stringify(data));
            }
        });

        res.status(200).send({ status: 'ok' });
    } catch (err) {
        console.error(err);
        res.status(500).send({ error: 'Failed to process telemetry' });
    }
});

const PORT = 3000;
server.listen(PORT, '0.0.0.0', () => {
    console.log(`Server is running on http://0.0.0.0:${PORT}`);
    console.log(`WebSocket server is running on ws://0.0.0.0:${PORT}`);
});

// Catch any completely unhandled exceptions so the server never goes down!
process.on('uncaughtException', (err) => {
    console.error('CRITICAL UNCAUGHT EXCEPTION:', err);
});
process.on('unhandledRejection', (reason, promise) => {
    console.error('Unhandled Rejection at:', promise, 'reason:', reason);
});
