/**
 *  Radu Sorin-Gabriel
 *  Grupa 410 - M1
 **/
import java.net.*; 
import java.io.*; 
  
public class Client 
{ 
    private Socket socket            = null; 
    private BufferedReader  input   = null; 
    private BufferedReader  in   = null; 
    private BufferedWriter out     = null; 
  
    // Creez client-ul pe IP-ul si port-ul dat
    public Client(InetAddress address, int port) 
    { 
        // Ma conectez la IP pe port-ul dat creand un socket
	// Creez stream-urile aferente pentru citire si scriere
        try
        { 
            socket = new Socket(address, port); 
            System.out.println("Connected"); 
  
            // takes input from terminal 
            input  = new BufferedReader(new InputStreamReader(System.in)); 
            
            in = new BufferedReader(new InputStreamReader(socket.getInputStream()));
  
            // sends output to the socket 
            out    = new BufferedWriter(new OutputStreamWriter(socket.getOutputStream())); 
        } 
        catch(UnknownHostException u) 
        { 
            System.out.println(u); 
        } 
        catch(IOException i) 
        { 
            System.out.println(i); 
        } 
    }
    
    public void start() {
  
        String command = "";
        
	// Pornesc thread-ul care citeste raspunsurile de la server
        ReadThread readThread = new ReadThread(in);
        readThread.start();
  
        // Cat timp comanda citita nu e exit
        while (!command.equals("exit")) 
        { 
            try
            { 
            	// Citesc comanda si o scriu in stream-ul de iesire
            	command = input.readLine(); 
            	out.write(command);
                out.flush();
		// Daca comanda este exit, opresc si thread-ul care citeste ce vine de la server
                if (command.equals("exit"))
                	readThread.stopThread();
            }
            catch(IOException i) 
            { 
                System.out.println(i); 
            } 
            
            
        } 
  
        // Inchid stream-urile si socket-ul
        try
        { 
            input.close();
            in.close();
            out.close(); 
            socket.close();
            readThread.join();
        } 
        catch(Exception i) 
        { 
            System.out.println(i); 
        } 
    } 
  
    public static void main(String args[]) 
    { 
    	final int PORT = 8080;
        try {
			Client client = new Client(InetAddress.getByName("localhost"), PORT);
			client.start();
		} catch (Exception e) {
			// TODO Auto-generated catch block
			e.printStackTrace();
		}
        
    } 
} 