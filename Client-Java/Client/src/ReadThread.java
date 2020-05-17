import java.io.BufferedReader;
import java.io.IOException;

public class ReadThread extends Thread{
	private BufferedReader  in;
	private boolean stop = false;

	public ReadThread(BufferedReader in) {
		this.in = in;
	}
	
	@Override
	public void run()
	{
		// Cat timp semnalul nu este de oprire
		while (!stop) 
		{
			// Citesc bucati de 1024 de caractere care vin de la server
			char[] buffer = new char[1024];
	        int read;
	        try {
				while (true)
				{
					try
					{
						read = in.read(buffer);
					}
					catch (IOException ex)
					{
						break;
					}
					// Daca am ajuns la finalul stream-ului, afisez ce am citit
					if (read != -1)
					{
						String result = new String(buffer, 0, read);
						System.out.println(result);
						if (buffer[read] == '\0')
							break;
					}
				}
			} catch (Exception e) {
				// TODO Auto-generated catch block
				e.printStackTrace();
			}
		}
	}
	
	public void stopThread()
	{
		stop = true;	
	}
	
	
}
